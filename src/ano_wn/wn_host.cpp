/* ============================================================================
 * hcomm_probe.cpp — 任务 14-a：hcomm channel 单机建链 + 队列内存归属实证
 * 子命令：
 *   server <aclDev> <urmaDevIdx> <devPhyId>   — SERVER 角色（本端 listen port）
 *   client <aclDev> <urmaDevIdx> <devPhyId>   — CLIENT 角色（对端 listen port）
 * 流程（shmem device_udma_transport_manager.cpp :2031-2057/:1442-1483/:1541 形态）：
 *   aclInit → urma ctx 取本端 EID → HcommEndpointCreate(UB_CTP/EID/DEVICE) →
 *   GetListenPort → aclrtMalloc 4KB + HcommMemReg(DEVICE) → 文件交换
 *   {EndpointDesc, port} → HcommChannelCreate(COMM_ENGINE_AIV) →
 *   GetStatus 轮询 READY → B2：D2H ChannelEntity 快照（SQ/CQ/db/token）→
 *   maps 归属判定 → 信息落盘（AIV 探针用）→ 守候 → 销毁。
 * ChannelEntity 布局 = CANN 树 pto/comm/async/urma/urma_hccl_defs.hpp（13-a 在案）。
 * ========================================================================= */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <time.h>

#include <acl/acl.h>
#include <aclnn/aclnn_base.h>
#include "aclnn_ano_probe.h"
#include <ub/umdk/urma/urma_api.h>

/* ---- AnoProbe 进程内探针面（12-b n2_aiv_host.cpp 同款机器，进程内复刻；算子历史名 N2AivProbe） ---- */
struct hp_probe_params {
	uint64_t addr;
	uint64_t addr2;
	uint64_t magic;
	uint32_t len;
	uint32_t mode;
	uint64_t wqe[8];
};
struct hp_probe_out {
	uint64_t done_magic;
	uint32_t mode_echo;
	uint32_t len_echo;
	uint8_t data[64];
};
#define HP_DONE_MAGIC 0x4E324149565030ull

static aclTensor *
hp_mk(void *dev, int64_t bytes)
{
	int64_t shape[1] = { bytes };
	return aclCreateTensor(shape, 1, ACL_UINT8, NULL, 0,
			       aclFormat::ACL_FORMAT_ND, shape, 1, dev);
}

/* 单点读探针：rc==0 且 done_magic 正确 = AIV 可达；否则不可达（断点） */
static int
hp_probe_read(void *d_params, void *d_out, aclrtStream stream, uint64_t addr, const char *name)
{
	struct hp_probe_params hp;
	struct hp_probe_out ho;
	memset(&hp, 0, sizeof(hp));
	memset(&ho, 0, sizeof(ho));
	hp.addr = addr;
	hp.len = 16;
	hp.mode = 0;
	if (aclrtMemcpy(d_params, sizeof(hp), &hp, sizeof(hp), ACL_MEMCPY_HOST_TO_DEVICE) != 0)
		return -1;
	if (aclrtMemset(d_out, 256, 0, 256) != 0)
		return -1;
	uint64_t ws = 0;
	aclOpExecutor *exec = NULL;
	aclTensor *tp = hp_mk(d_params, 256);
	aclTensor *to = hp_mk(d_out, 256);
	int rc = (int)aclnnAnoProbeGetWorkspaceSize(tp, to, &ws, &exec);
	if (rc == 0)
		rc = (int)aclnnAnoProbe(NULL, ws, exec, stream);
	if (rc == 0)
		rc = (int)aclrtSynchronizeStream(stream);
	if (rc == 0)
		rc = (int)aclrtMemcpy(&ho, sizeof(ho), d_out, sizeof(ho), ACL_MEMCPY_DEVICE_TO_HOST);
	aclDestroyTensor(tp);
	aclDestroyTensor(to);
	if (rc == 0 && ho.done_magic != HP_DONE_MAGIC)
		rc = -2;
	printf("PROBEADDR %s @0x%llx => rc=%d %s data=%02x%02x%02x%02x\n", name,
	       (unsigned long long)addr, rc, rc == 0 ? "AIV-REACHABLE" : "AIV-UNREACHABLE",
	       ho.data[0], ho.data[1], ho.data[2], ho.data[3]);
	return rc;
}

/* rtGetDeviceInfo（libruntime 导出 T 符号，nm -D 实证；shmem
 * dl_comm_def.h:25-31 的 infoType 枚举：SDID=26 SERVER_ID=27 SUPER_POD_ID=29） */
extern "C" int rtGetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *val);

#include "hcomm/hcomm_res.h"
#include "hcomm/hcomm_channel.h"
#include "pto/comm/async/urma/urma_hccl_defs.hpp"
#include "n2_udma_internal.h"	/* n2_jfs_sqe_ctl/n2_wqe_sge/n2_jfc_cqe 布局（12-b 在案） */

/* ---- 任务 12-c：AIV 物理发射面（mode3：WQE 上环 + dbVa 直敲 + head 影子双写） ---- */
static int g_sendsq = 0;
static int g_wnfire = 0;
static int g_wniters = 1;
static int g_bench_iters = 0;	/* 23b bench 模式：>0 时 client 跑 N 次发射-收割循环 */
static uint32_t g_bench_xlen = 4096;

static uint64_t
hp_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

struct hp_fire_params {
	uint64_t addr;		/* SQ 槽位地址（sqVa + slot*64） */
	uint64_t addr2;		/* dbVa（门铃直写） */
	uint64_t addr3;		/* SQ head 影子地址（PTO 双写形态） */
	uint64_t magic;		/* 门铃值 = head+1 */
	uint32_t len;
	uint32_t mode;		/* 3 */
	uint64_t wqe[8];
};

/* AIV mode3 发射（kernel：WQE→flush→PipeBarrier→dbVa+head 影子双写） */
static int
hp_fire(void *d_params, void *d_out, aclrtStream stream,
	uint64_t sq_slot, uint64_t db_va, uint64_t head_addr, uint32_t head_next,
	const uint8_t wqe[64])
{
	struct hp_fire_params hp;
	memset(&hp, 0, sizeof(hp));
	hp.addr = sq_slot;
	hp.addr2 = db_va;
	hp.addr3 = head_addr;
	hp.magic = head_next;
	hp.mode = 3;
	memcpy(hp.wqe, wqe, 64);
	if (aclrtMemcpy(d_params, sizeof(hp), &hp, sizeof(hp), ACL_MEMCPY_HOST_TO_DEVICE) != 0)
		return -1;
	if (aclrtMemset(d_out, 256, 0, 256) != 0)
		return -1;
	uint64_t ws = 0;
	aclOpExecutor *exec = NULL;
	aclTensor *tp = hp_mk(d_params, 256);
	aclTensor *to = hp_mk(d_out, 256);
	int rc = (int)aclnnAnoProbeGetWorkspaceSize(tp, to, &ws, &exec);
	if (rc == 0)
		rc = (int)aclnnAnoProbe(NULL, ws, exec, stream);
	if (rc == 0)
		rc = (int)aclrtSynchronizeStream(stream);
	aclDestroyTensor(tp);
	aclDestroyTensor(to);
	return rc;
}

/* mode1 无害写（写 8B 全 0 到 dbVa——门铃值 0=无新工作，不触发硬件动作） */
static int
hp_harmless_db_write(void *d_params, void *d_out, aclrtStream stream, uint64_t db_va)
{
	struct hp_fire_params hp;
	memset(&hp, 0, sizeof(hp));
	hp.addr = db_va;
	hp.magic = 0;
	hp.len = 8;
	hp.mode = 1;
	if (aclrtMemcpy(d_params, sizeof(hp), &hp, sizeof(hp), ACL_MEMCPY_HOST_TO_DEVICE) != 0)
		return -1;
	if (aclrtMemset(d_out, 256, 0, 256) != 0)
		return -1;
	uint64_t ws = 0;
	aclOpExecutor *exec = NULL;
	aclTensor *tp = hp_mk(d_params, 256);
	aclTensor *to = hp_mk(d_out, 256);
	int rc = (int)aclnnAnoProbeGetWorkspaceSize(tp, to, &ws, &exec);
	if (rc == 0)
		rc = (int)aclnnAnoProbe(NULL, ws, exec, stream);
	if (rc == 0)
		rc = (int)aclrtSynchronizeStream(stream);
	aclDestroyTensor(tp);
	aclDestroyTensor(to);
	return rc;
}

/* AIV 读回数据变体（取 CQE 内容用；rc==0 时 out 填前 len 字节） */
static int
hp_read_data(void *d_params, void *d_out, aclrtStream stream, uint64_t addr,
	     uint32_t len, uint8_t *out)
{
	struct hp_fire_params hp;
	struct hp_probe_out ho;
	memset(&hp, 0, sizeof(hp));
	memset(&ho, 0, sizeof(ho));
	hp.addr = addr;
	hp.len = len > 64 ? 64 : len;
	hp.mode = 0;
	if (aclrtMemcpy(d_params, sizeof(hp), &hp, sizeof(hp), ACL_MEMCPY_HOST_TO_DEVICE) != 0)
		return -1;
	if (aclrtMemset(d_out, 256, 0, 256) != 0)
		return -1;
	uint64_t ws = 0;
	aclOpExecutor *exec = NULL;
	aclTensor *tp = hp_mk(d_params, 256);
	aclTensor *to = hp_mk(d_out, 256);
	int rc = (int)aclnnAnoProbeGetWorkspaceSize(tp, to, &ws, &exec);
	if (rc == 0)
		rc = (int)aclnnAnoProbe(NULL, ws, exec, stream);
	if (rc == 0)
		rc = (int)aclrtSynchronizeStream(stream);
	if (rc == 0)
		rc = (int)aclrtMemcpy(&ho, sizeof(ho), d_out, sizeof(ho), ACL_MEMCPY_DEVICE_TO_HOST);
	aclDestroyTensor(tp);
	aclDestroyTensor(to);
	if (rc == 0 && ho.done_magic != HP_DONE_MAGIC)
		return -2;
	if (rc == 0)
		memcpy(out, ho.data, hp.len);
	return rc;
}


#define SRV_OOB  "/tmp/hcomm_srv_oob_lijiawen.bin"
#define CLI_OOB  "/tmp/hcomm_cli_oob_lijiawen.bin"
#define SRV_DONE "/tmp/hcomm_srv_done_lijiawen"
#define CLI_DONE "/tmp/hcomm_cli_done_lijiawen"
#define STOPF    "/tmp/hcomm_probe_stop_lijiawen"
#define INFO_OUT "/tmp/hcomm_chan_info_lijiawen.txt"

struct oob_rec {
	EndpointDesc desc;
	uint32_t listen_port;
};

static int
wait_file(const char *path, int sec)
{
	for (int i = 0; i < sec * 10; i++) {
		if (access(path, F_OK) == 0)
			return 0;
		usleep(100 * 1000);
	}
	return -1;
}

static void
touch(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f) { fputs("ok\n", f); fclose(f); }
}

static void
maps_line(const char *name, uint64_t addr)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	int found = 0;
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		uint64_t lo, hi;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && addr >= lo && addr < hi) {
			line[strcspn(line, "\n")] = 0;
			printf("  maps[%s 0x%llx]: %s\n", name, (unsigned long long)addr, line);
			found = 1;
			break;
		}
	}
	if (!found)
		printf("  maps[%s 0x%llx]: <不在本进程 maps>（device 侧内存特征）\n",
		       name, (unsigned long long)addr);
	fclose(f);
}

static int
run_side(int is_server, int acl_dev, int urma_idx, uint32_t phy_id_arg, uint32_t fire_tid, int engine_arg)
{
	/* 1. acl + urma（本端 EID） */
	if (aclInit(NULL) != ACL_SUCCESS) { printf("FAIL aclInit\n"); return 2; }
	if (aclrtSetDevice(acl_dev) != ACL_SUCCESS) { printf("FAIL SetDevice\n"); return 2; }

	urma_init_attr_t ia = { .token = 0, .uasid = 0 };
	urma_status_t ust = urma_init(&ia);
	if (ust != URMA_SUCCESS && ust != URMA_EEXIST) { printf("FAIL urma_init %d\n", ust); return 1; }
	int num = 0;
	urma_device_t **list = urma_get_device_list(&num);
	if (!list || num <= urma_idx) { printf("FAIL urma 设备数 %d\n", num); return 1; }
	urma_context_t *uctx = urma_create_context(list[urma_idx], 0);
	urma_free_device_list(list);
	if (!uctx) { printf("FAIL urma ctx\n"); return 1; }
	char eidstr[80];
	snprintf(eidstr, sizeof(eidstr),
		 "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
		 uctx->eid.raw[0], uctx->eid.raw[1], uctx->eid.raw[2], uctx->eid.raw[3],
		 uctx->eid.raw[4], uctx->eid.raw[5], uctx->eid.raw[6], uctx->eid.raw[7],
		 uctx->eid.raw[8], uctx->eid.raw[9], uctx->eid.raw[10], uctx->eid.raw[11],
		 uctx->eid.raw[12], uctx->eid.raw[13], uctx->eid.raw[14], uctx->eid.raw[15]);
	printf("OK 本端 EID=%s（urma 设备 idx=%d，共 %d）\n", eidstr, urma_idx, num);

	/* 2. endpoint（shmem :2031-2052 形态；desc 取驱动预置件——首轮手工 EID
	 * （urma ctx eid 0400/df08 档）RaUbCtx init -22 实证被拒，驱动枚举的
	 * 0300/df00 档才是 hcomm 接受面；loc 实值查询保留作记录） */
	int32_t phy_id = (int32_t)phy_id_arg;
	(void)phy_id_arg;
	if (aclrtGetPhyDevIdByLogicDevId(acl_dev, &phy_id) != ACL_SUCCESS)
		printf("WARN aclrtGetPhyDevIdByLogicDevId 失败，沿用入参 %d\n", phy_id);

	EndpointDesc edesc;
	uint32_t dnum = 0;
	HcommResult dhr = HcommEndpointGetDescNum(acl_dev, &dnum);
	if (dhr != 0 || dnum == 0) { printf("FAIL GetDescNum ret=%d num=%u\n", (int)dhr, dnum); return 1; }
	EndpointDesc *dlist = (EndpointDesc *)calloc(dnum, sizeof(EndpointDesc));
	uint32_t cap = dnum;
	dhr = HcommEndpointGetDescs(acl_dev, &cap, dlist);
	if (dhr != 0 || cap == 0) { printf("FAIL GetDescs ret=%d\n", (int)dhr); return 1; }
	int picked = -1;
	for (uint32_t i = 0; i < cap; i++) {
		if (dlist[i].protocol == COMM_PROTOCOL_UBC_CTP &&
		    dlist[i].commAddr.type == COMM_ADDR_TYPE_EID) { picked = (int)i; break; }
	}
	if (picked < 0) { printf("FAIL 无 UB_CTP 预置 endpoint\n"); return 1; }
	edesc = dlist[picked];
	free(dlist);
	printf("OK 取驱动预置 desc[%d]：eid=%02x%02x:..:%02x%02x loc.phy=%u\n", picked,
	       edesc.commAddr.eid[0], edesc.commAddr.eid[1],
	       edesc.commAddr.eid[14], edesc.commAddr.eid[15], edesc.loc.device.devPhyId);
	EndpointHandle ep = NULL;
	HcommResult hr = HcommEndpointCreate(&edesc, &ep);
	if (hr != 0 || ep == NULL) { printf("FAIL HcommEndpointCreate ret=%d\n", (int)hr); return 1; }
	uint32_t lport = 0;
	hr = HcommEndpointGetListenPort(ep, &lport);
	printf("OK endpoint=%p listen_port=%u（ret=%d）\n", ep, lport, (int)hr);

	/* 3. device mem + HcommMemReg */
	void *dbuf = NULL;
	if (aclrtMalloc(&dbuf, 4096, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
		printf("FAIL aclrtMalloc\n"); return 2;
	}
	CommMem cm;
	cm.type = COMM_MEM_TYPE_DEVICE;
	cm.addr = dbuf;
	cm.size = 4096;
	HcommMemHandle mh = NULL;
	hr = HcommMemReg(ep, "n14sym", &cm, &mh);
	if (hr != 0) { printf("FAIL HcommMemReg ret=%d\n", (int)hr); return 1; }
	printf("OK memreg handle=%p（DEVICE 4KB @%p）\n", mh, dbuf);

	/* server：布置 4KB 递增图案（12-c 发射的读目标；aclrtMemcpy H2D——
	 * 首轮 host VA 直写 davinci_manager 映射页 core dump 实证不可直写） */
	if (is_server) {
		uint32_t pat[1024];
		for (uint32_t i = 0; i < 1024; i++)
			pat[i] = i;
		if (aclrtMemcpy(dbuf, 4096, pat, 4096, ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
			printf("FAIL 图案 H2D\n");
			return 2;
		}
		printf("OK 图案布置完成（server buf w[i]=i ×1024，H2D）\n");
	}

	/* 4. OOB 交换 */
	struct oob_rec self_rec, peer_rec;
	self_rec.desc = edesc;
	self_rec.listen_port = lport;
	FILE *f = fopen(is_server ? SRV_OOB : CLI_OOB, "wb");
	if (!f) { printf("FAIL oob write\n"); return 1; }
	fwrite(&self_rec, 1, sizeof(self_rec), f);
	fclose(f);
	touch(is_server ? SRV_DONE : CLI_DONE);
	if (wait_file(is_server ? CLI_DONE : SRV_DONE, 120) != 0) { printf("FAIL 等对端\n"); return 1; }
	f = fopen(is_server ? CLI_OOB : SRV_OOB, "rb");
	if (!f || fread(&peer_rec, 1, sizeof(peer_rec), f) != sizeof(peer_rec)) {
		printf("FAIL oob read\n"); return 1;
	}
	if (f) fclose(f);
	printf("OK 对端 desc 到手（其对端 port=%u）\n", peer_rec.listen_port);

	/* 5. channel desc + create（shmem :1442-1483 形态） */
	HcommChannelDesc cdesc;
	HcommChannelDescInit(&cdesc, 1);
	cdesc.remoteEndpoint = peer_rec.desc;
	cdesc.notifyNum = 0;
	cdesc.exchangeAllMems = false;
	HcommMemHandle handles[1] = { mh };
	cdesc.memHandles = handles;
	cdesc.memHandleNum = 1;
	cdesc.role = is_server ? HCOMM_SOCKET_ROLE_SERVER : HCOMM_SOCKET_ROLE_CLIENT;
	cdesc.port = is_server ? (uint16_t)lport : (uint16_t)peer_rec.listen_port;

	ChannelHandle chan = 0;
	/* 23b：引擎可配（默认 AIV=既有行为逐值不变）；对拍基线需 AICPU_TS 对称 */
	CommEngine engine = (CommEngine)engine_arg;
	hr = HcommChannelCreate(ep, engine, &cdesc, 1, &chan);
	printf("HcommChannelCreate ret=%d handle=0x%llx\n", (int)hr, (unsigned long long)chan);
	if (hr != 0 || chan == 0) { printf("FAIL channel create\n"); return 1; }

	/* 6. status 轮询（shmem WaitHcommChannelReady 语义） */
	int32_t st = -1;
	int ready = 0;
	for (int i = 0; i < 600; i++) {
		hr = HcommChannelGetStatus(&chan, 1, &st);
		if (hr != 0 || st == HCOMM_CHANNEL_STATUS_FAILED_INTERNAL)
			break;
		if (st == HCOMM_CHANNEL_STATUS_READY) { ready = 1; break; }
		usleep(50 * 1000);
	}
	printf("ChannelGetStatus ret=%d status=%d（%s）\n", (int)hr, (int)st,
	       ready ? "READY" : "未就绪");
	if (!ready)
		return 1;

	/* 7. B2：ChannelEntity D2H 快照（PTO urma_channel_helper.hpp:103-118 形态） */
	ChannelEntity ent;
	memset(&ent, 0, sizeof(ent));
	if (aclrtMemcpy(&ent, sizeof(ent), (void *)(uintptr_t)chan, sizeof(ent),
			ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
		printf("FAIL D2H ChannelEntity（handle 或不是 device 指针？如实记录）\n");
		return 1;
	}
	printf("=== ChannelEntity：magic=0x%x engine=%d protocol=%d sqNum=%u cqNum=%u "
	       "localBufNum=%u remoteBufNum=%u ===\n",
	       ent.abiHeader.magicWord, (int)ent.engine, (int)ent.protocol,
	       ent.sqNum, ent.cqNum, ent.localBufferNum, ent.remoteBufferNum);

	SqContext sq;
	CqContext cq;
	memset(&sq, 0, sizeof(sq));
	memset(&cq, 0, sizeof(cq));
	if (ent.sqContextAddr && ent.sqNum > 0)
		aclrtMemcpy(&sq, sizeof(sq), ent.sqContextAddr, sizeof(sq), ACL_MEMCPY_DEVICE_TO_HOST);
	if (ent.cqContextAddr && ent.cqNum > 0)
		aclrtMemcpy(&cq, sizeof(cq), ent.cqContextAddr, sizeof(cq), ACL_MEMCPY_DEVICE_TO_HOST);
	printf("=== SQ ubJfs：sqVa=0x%llx headAddr=0x%llx tailAddr=0x%llx dbVa=0x%llx\n"
	       "    jfsID=%u wqeSize=%u sqDepth=%u tpID=%u rmtEID=%02x%02x..%02x%02x ===\n",
	       (unsigned long long)sq.contextInfo.ubJfs.sqVa,
	       (unsigned long long)sq.contextInfo.ubJfs.headAddr,
	       (unsigned long long)sq.contextInfo.ubJfs.tailAddr,
	       (unsigned long long)sq.contextInfo.ubJfs.dbVa,
	       sq.contextInfo.ubJfs.jfsID, sq.contextInfo.ubJfs.wqeSize,
	       sq.contextInfo.ubJfs.sqDepth, sq.contextInfo.ubJfs.tpID,
	       sq.contextInfo.ubJfs.remoteEID[0], sq.contextInfo.ubJfs.remoteEID[1],
	       sq.contextInfo.ubJfs.remoteEID[14], sq.contextInfo.ubJfs.remoteEID[15]);
	printf("=== CQ ubJfc：scqVa=0x%llx headAddr=0x%llx tailAddr=0x%llx dbVa=0x%llx "
	       "jfcID=%u cqeSize=%u cqDepth=%u ===\n",
	       (unsigned long long)cq.contextInfo.ubJfc.scqVa,
	       (unsigned long long)cq.contextInfo.ubJfc.headAddr,
	       (unsigned long long)cq.contextInfo.ubJfc.tailAddr,
	       (unsigned long long)cq.contextInfo.ubJfc.dbVa,
	       cq.contextInfo.ubJfc.jfcID, cq.contextInfo.ubJfc.cqeSize,
	       cq.contextInfo.ubJfc.cqDepth);

	/* B3：token 快照（ProtectionInfo.ub） */
	RegedBufferEntity lb, rb;
	memset(&lb, 0, sizeof(lb));
	memset(&rb, 0, sizeof(rb));
	if (ent.localBufferAddr && ent.localBufferNum > 0) {
		aclrtMemcpy(&lb, sizeof(lb), ent.localBufferAddr, sizeof(lb), ACL_MEMCPY_DEVICE_TO_HOST);
		printf("=== localBuf[0]：type=%d addr=0x%llx size=%llu tokenId=%u tokenValue=0x%x ===\n",
		       lb.type, (unsigned long long)lb.bufferInfo.rma.addr,
		       (unsigned long long)lb.bufferInfo.rma.size,
		       lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId,
		       lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue);
	}
	if (ent.remoteBufferAddr && ent.remoteBufferNum > 0) {
		aclrtMemcpy(&rb, sizeof(rb), ent.remoteBufferAddr, sizeof(rb), ACL_MEMCPY_DEVICE_TO_HOST);
		printf("=== remoteBuf[0]：type=%d addr=0x%llx size=%llu tokenId=%u tokenValue=0x%x ===\n",
		       rb.type, (unsigned long long)rb.bufferInfo.rma.addr,
		       (unsigned long long)rb.bufferInfo.rma.size,
		       rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId,
		       rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue);
	}

	/* 8. maps 归属判定 */
	maps_line("channel handle/entity", (uint64_t)chan);
	maps_line("sqVa", sq.contextInfo.ubJfs.sqVa);
	maps_line("sq.dbVa", sq.contextInfo.ubJfs.dbVa);
	maps_line("cq.scqVa", cq.contextInfo.ubJfc.scqVa);
	maps_line("devBuf(aclrtMalloc)", (uint64_t)(uintptr_t)dbuf);

	/* 9. 信息落盘（AIV 探针用） */
	FILE *of = fopen(INFO_OUT, "w");
	if (of) {
		fprintf(of, "sqVa=0x%llx\ndbVa=0x%llx\nscqVa=0x%llx\ndevBuf=0x%llx\n"
			"sqDepth=%u\nwqeSize=%u\njfsID=%u\ntpID=%u\n"
			"lcl_tokenId=%u\nlcl_tokenValue=0x%x\nrmt_tokenId=%u\nrmt_tokenValue=0x%x\n",
			(unsigned long long)sq.contextInfo.ubJfs.sqVa,
			(unsigned long long)sq.contextInfo.ubJfs.dbVa,
			(unsigned long long)cq.contextInfo.ubJfc.scqVa,
			(unsigned long long)(uintptr_t)dbuf,
			sq.contextInfo.ubJfs.sqDepth, sq.contextInfo.ubJfs.wqeSize,
			sq.contextInfo.ubJfs.jfsID, sq.contextInfo.ubJfs.tpID,
			lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId,
			lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue,
			rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId,
			rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue);
		fclose(of);
	}
	printf("=== 建链+快照完成 ===\n");
	/* B2 第三板斧（进程内修正版）：channel 创建进程的 device 上下文中
	 * 直读各队列地址——跨进程探测已被证无效（12-b 同手法在案） */
	aclrtStream pstream = NULL;
	void *d_params = NULL, *d_out = NULL;
	{
		if (aclrtCreateStream(&pstream) == ACL_SUCCESS &&
		    aclrtMalloc(&d_params, 256, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS &&
		    aclrtMalloc(&d_out, 256, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS) {
			hp_probe_read(d_params, d_out, pstream, (uint64_t)(uintptr_t)dbuf, "devBuf(in-proc)");
			hp_probe_read(d_params, d_out, pstream, sq.contextInfo.ubJfs.sqVa, "sqVa(in-proc)");
			hp_probe_read(d_params, d_out, pstream, cq.contextInfo.ubJfc.scqVa, "scqVa(in-proc)");
			hp_probe_read(d_params, d_out, pstream, sq.contextInfo.ubJfs.dbVa, "dbVa(in-proc)");
			hp_probe_read(d_params, d_out, pstream, (uint64_t)chan, "chanEntity(in-proc)");
		} else {
			printf("WARN 探针资源分配失败\n");
		}
	}
	printf("=== 守候销毁信号（%s）===\n", is_server ? "server" : "client");

	/* ================= 任务 39：WRITE_WITH_NOTIFY 命令面（wnfire 子命令，client 侧） =================
	/* 2-WQEBB 形态（任务 36 对拍在案）：slot0=WQE ctl48B+NotifyCtx 前 16B，slot1=NotifyCtx 后 16B+SGE。
	 * 发射纪律：slot0 先写+无害门铃（magic=0，12-c stage0 在案），slot1 写完才落真门铃（head+2）；
	 * flag=0x2D（URMA_DEFAULT_CFG 官方默认：relax 序+fence+cqe，inner_def.h:55-61 在案）。 */
	if (g_wnfire && !is_server) {
		uint32_t ltid = lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;
		uint32_t rtid = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;
		uint32_t rtval = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue;
		uint64_t rmt = rb.bufferInfo.rma.addr;
		uint64_t lva = (uint64_t)(uintptr_t)dbuf;
		uint64_t inbox = rmt + 0x800;	/* 对端命令收件箱槽环基址（tgt_wn 约定：8 槽×128B） */
		uint64_t cqe_va = lva + 0x100;
		uint64_t data_va = lva + 0x200;
		uint64_t sqd = sq.contextInfo.ubJfs.sqDepth;
		uint64_t cqd = cq.contextInfo.ubJfc.cqDepth;
		printf("=== wnfire：WRITE_WITH_NOTIFY（sqVa=0x%llx dbVa=0x%llx inbox=0x%llx rtid=%u ltid=%u iters=%d） ===\n",
		       (unsigned long long)sq.contextInfo.ubJfs.sqVa,
		       (unsigned long long)sq.contextInfo.ubJfs.dbVa,
		       (unsigned long long)inbox, rtid, ltid, g_wniters);
		uint32_t head0 = 0;
		uint8_t u32buf[4] = {0};
		if (hp_read_data(d_params, d_out, pstream, sq.contextInfo.ubJfs.headAddr, 4, u32buf) == 0)
			head0 = *(uint32_t *)u32buf;
		uint32_t ok = 0;
		uint32_t rok = 0;	/* 任务 40：AIV 图内回执收割成功计数（判据=AIV 侧读数） */
		for (int it = 0; it < g_wniters; it++) {
			uint32_t cid = (uint32_t)it % 8u;
			uint32_t head = head0 + (uint32_t)it * 2u;
			/* 1) 104B 载荷（8B 控制头 valid+cid + 96B SQE；SQE 字段与任务 33 逐字节一致） */
			uint8_t pay[104];
			memset(pay, 0, sizeof(pay));
			pay[0] = 1;
			pay[1] = (uint8_t)cid;
			uint32_t *dw = (uint32_t *)(pay + 8);
			dw[0] = 0x02;
			dw[1] = 1;
			uint8_t *sgl = pay + 8 + 24;
			*(uint64_t *)sgl = data_va;
			sgl[8] = 0x00; sgl[9] = 0x01; sgl[10] = 0x00;
			*(uint32_t *)(sgl + 11) = ltid;
			sgl[15] = 0x04;
			dw[10] = 8; dw[11] = 0;
			dw[22] = 0x1001u | (1u << 16);
			dw[23] = 0xACFEu;
			if (aclrtMemcpy((char *)dbuf + 0x400, sizeof(pay), pay, sizeof(pay),
					ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
				printf("FAIL 载荷 H2D\n"); return 2;
			}
			/* 2) 128B WQE 镜像（ctl48 + NotifyCtx32 + SGE16 + pad32） */
			uint8_t wqe[128];
			memset(wqe, 0, sizeof(wqe));
			uint32_t *wd = (uint32_t *)wqe;
			uint64_t slot_off = inbox + (uint64_t)cid * 128u;
			uint32_t owner = (((uint64_t)head & sqd) == 0u) ? 1u : 0u;	/* 官方 lap-parity（jetty.h:97 口径） */
			wd[0] = ((head + 2) & 0xFFFFu) | (0x2Du << 16) | (1u << 28) | (1u << 29) | (owner << 31);
			wd[1] = (5u << 8);				/* opcode=WRITE_WITH_NOTIFY=5（urma_types.hpp:77） */
			wd[2] = sq.contextInfo.ubJfs.tpID | (1u << 24);
			wd[3] = rtid & 0xFFFFFu;
			memcpy(wqe + 16, sq.contextInfo.ubJfs.remoteEID, 16);
			wd[8] = rtval;
			wd[10] = (uint32_t)(slot_off & 0xFFFFFFFFull);
			wd[11] = (uint32_t)(slot_off >> 32);
			/* NotifyCtx（HcommUrmaNotifyCtx，inner_def.h:523-532；落点/收割语义标"语义待厂商"） */
			wd[12] = rtid & 0xFFFFFu;			/* notifyTokenId */
			wd[13] = rtval;					/* notifyTokenValue */
			wd[14] = (uint32_t)(slot_off & 0xFFFFFFFFull);	/* notifyAddrL=槽控制头 */
			wd[15] = (uint32_t)(slot_off >> 32);		/* notifyAddrH */
			wd[16] = 1; wd[17] = 0;			/* notifyData=1（u64 写入 notifyAddr=valid 标志；39 实证 notify=数据写） */
			/* SGE（dw20-23） */
			wd[20] = 104;
			wd[21] = ltid;
			*(uint64_t *)(wqe + 88) = lva + 0x400;
			/* 3) 双写上环：slot0（magic=0 无害）→ slot1（magic=head+2 真门铃） */
			uint64_t slotVa0 = sq.contextInfo.ubJfs.sqVa + (uint64_t)(head % sqd) * 64u;
			uint64_t slotVa1 = sq.contextInfo.ubJfs.sqVa + (uint64_t)((head + 1) % sqd) * 64u;
			int frc = hp_fire(d_params, d_out, pstream, slotVa0,
					  sq.contextInfo.ubJfs.dbVa, sq.contextInfo.ubJfs.headAddr, 0, wqe);
			if (frc == 0)
				frc = hp_fire(d_params, d_out, pstream, slotVa1,
					      sq.contextInfo.ubJfs.dbVa, sq.contextInfo.ubJfs.headAddr, head + 2, wqe + 64);
			printf("  wnfire：第 %d 发 cid=%u head=%u→%u rc=%d（slot0=0x%llx slot1=0x%llx）\n",
			       it, cid, head, head + 2, frc,
			       (unsigned long long)slotVa0, (unsigned long long)slotVa1);
			/* 4) 收割发送侧 CQE（owner-bit，第 it 槽） */
			uint64_t cq_slot = cq.contextInfo.ubJfc.scqVa + (uint64_t)((head0 + (uint32_t)it) % cqd) * 64u;
			uint32_t exp_owner = ((((head0 + (uint32_t)it) / cqd) % 2u) == 0u) ? 1u : 0u;
			int got = 0;
			for (int w = 0; w < 5000; w++) {
				uint8_t pb[4] = {0};
				if (hp_read_data(d_params, d_out, pstream, cq_slot, 4, pb) == 0 &&
				    (((*(uint32_t *)pb) >> 2) & 1u) == exp_owner) { got = 1; break; }
				usleep(1000);
			}
			if (got) {
				uint8_t cqeraw[32];
				memset(cqeraw, 0, sizeof(cqeraw));
				hp_read_data(d_params, d_out, pstream, cq_slot, 32, cqeraw);
				struct n2_jfc_cqe *cqe = (struct n2_jfc_cqe *)cqeraw;
				printf("  wnfire：CQE status=%u substatus=%u owner=%u\n",
				       cqe->status, cqe->substatus, cqe->owner);
				if (cqe->status == 0)
					ok++;
			} else {
				printf("  wnfire：CQE 超时未见（如实记录）\n");
			}
			/* 4.5) 任务 40：AIV 图内回执收割——hp_read_data（AIV mode0 读 HBM）轮询
			 * 回执槽 valid→命中后 4×64B AIV 分片读取比对→mode1 st_dev 写 0 清位复用。
			 * 判据全部以 AIV 侧读数为准；host aclrtMemcpy 仅循环外降级观察帧。 */
			int rcpt_ok = 0;
			uint8_t vbuf[32];
			for (int w = 0; w < 800; w++) {
				memset(vbuf, 0, sizeof(vbuf));
				if (hp_read_data(d_params, d_out, pstream, cqe_va, 32, vbuf) == 0 &&
				    vbuf[16] == 1) { rcpt_ok = 1; break; }
				usleep(10000);
			}
			if (rcpt_ok) {
				uint32_t gotd[64];
				int dm = 1;
				for (int s = 0; s < 4; s++) {
					if (hp_read_data(d_params, d_out, pstream, data_va + (uint64_t)s * 64u,
							 64, (uint8_t *)gotd + (size_t)s * 64u) != 0) {
						dm = -1; break;
					}
				}
				if (dm == 1)
					for (int i = 0; i < 64; i++)
						if (gotd[i] != (uint32_t)i) { dm = 0; break; }
				hp_harmless_db_write(d_params, d_out, pstream, cqe_va + 16);	/* 清 valid（byte16 所在 u32） */
				printf("  wnfire：第 %d 发回执 AIV 收割 valid=1 cid=%u 数据=%s（已清位）\n",
				       it, cid, dm == 1 ? "MATCH" : (dm == 0 ? "MISMATCH" : "AIV 读失败"));
				if (dm == 1)
					rok++;
			} else {
				printf("  wnfire：第 %d 发回执 valid 超时未见（AIV 轮询 8s，如实记录）\n", it);
			}
		}
		/* 5) 降级观察帧：host aclrtMemcpy D2H 对照读数（打印用，非判据） */
		{
			uint8_t vb[32];
			uint32_t hd[4];
			memset(vb, 0, sizeof(vb));
			memset(hd, 0, sizeof(hd));
			aclrtMemcpy(vb, sizeof(vb), (char *)dbuf + 0x100, sizeof(vb), ACL_MEMCPY_DEVICE_TO_HOST);
			aclrtMemcpy(hd, sizeof(hd), (char *)dbuf + 0x200, sizeof(hd), ACL_MEMCPY_DEVICE_TO_HOST);
			printf("  wnfire：host 降级观察帧 cqe[16]=%u data[0..3]=%u,%u,%u,%u（非判据，末发清位后应为 0）\n",
			       vb[16], hd[0], hd[1], hd[2], hd[3]);
			printf("  [%s] wnfire 回执面 AIV 图内闭环（%u/%d 发 CQE=0 + %u/%d 发回执 AIV 侧 MATCH）\n",
			       (ok == (uint32_t)g_wniters && rok == (uint32_t)g_wniters) ? "PASS" : "FAIL",
			       ok, g_wniters, rok, g_wniters);
		}
		touch("/tmp/hcomm_fire_done_lijiawen");
	}

	/* ================= 任务 33：SEND 形态发起侧（sendsq 子命令，client 侧） =================
	/* 流程：96B SQE 布置于 dbuf+0 → SEND WQE（opcode=0，PTO UrmaOpcode::SEND）→
	 * hp_fire 发射+收割发送侧 CQE → 轮询 cqe_buf[0] valid → 数据 256B 比对。
	 * 布局口径：nvme_hcomm_sqe_96b.h:1-44（dw0-15 NVMe cmd / dw22 路由 / dw23 jetty_token /
	 * 尾部 keyed SGL：addr+0、len24@+8、key32@+11、type@+15）。 */
	if (g_sendsq && !is_server) {
		uint32_t ltid = lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;
		uint32_t rtid = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;
		uint32_t rtval = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue;
		uint64_t rmt = rb.bufferInfo.rma.addr;
		uint64_t lva = (uint64_t)(uintptr_t)dbuf;
		uint64_t cqe_va = lva + 0x100;	/* cqe_buf[0] 区（32B：cpl16B+valid@+16） */
		uint64_t data_va = lva + 0x200;	/* TGT 回写数据落区 256B */
		printf("=== sendsq：SEND 形态发起侧（sqVa=0x%llx dbVa=0x%llx rmt=0x%llx rtid=%u ltid=%u） ===\n",
		       (unsigned long long)sq.contextInfo.ubJfs.sqVa,
		       (unsigned long long)sq.contextInfo.ubJfs.dbVa,
		       (unsigned long long)rmt, rtid, ltid);
		/* 1) 96B SQE（host 预组，H2D 落 dbuf+0；slba = slba_base + idx × block_lbas = 0 + 1×8 = 8） */
		uint8_t sqe96[96];
		memset(sqe96, 0, sizeof(sqe96));
		uint32_t *dw = (uint32_t *)sqe96;
		dw[0] = 0x02;			/* NVMe Read 命令（opcode=0x02，nvme_spec） */
		dw[1] = 1;			/* nsid=1 */
		uint8_t *sgl = sqe96 + 24;	/* dptr.sgl1：keyed data block 描述符 */
		*(uint64_t *)sgl = data_va;	/* TGT 回写目标（client dbuf+0x200） */
		sgl[8] = 0x00; sgl[9] = 0x01; sgl[10] = 0x00;	/* len=256（24bit LE） */
		*(uint32_t *)(sgl + 11) = ltid;	/* key = 本端 tokenId（L4 凭证链不变） */
		sgl[15] = 0x04;			/* KEYED_DATA_BLOCK（10-f 在案 0x4） */
		dw[10] = 8; dw[11] = 0;		/* CDW10/11 = SLBA = 8（idx=1 算例，hov_xlate.h:17 公式） */
		dw[22] = 0x1001u | (1u << 16);	/* dword22 路由字：ctrlid=0x1001 | sqid=1（21 仿真体系） */
		dw[23] = 0xACFEu;			/* dword23 = jetty_token（0xACFE 在案约定） */
		if (aclrtMemcpy(dbuf, 96, sqe96, 96, ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
			printf("FAIL SQE H2D\n"); return 2;
		}
		aclrtMemset((char *)dbuf + 0x100, 32, 0, 32);
		aclrtMemset((char *)dbuf + 0x200, 256, 0xEE, 256);
		printf("  sendsq：96B SQE 已布置（dbuf+0；NVMe Read nsid=1 slba=8 len=256B key=%u；cid=0）\n", ltid);
		/* 2) SEND WQE（64B WQEBB：opcode=0 SEND；SGE=载荷源 dbuf+0 长 96） */
		uint32_t head0 = 0;
		uint8_t u32buf[4] = {0};
		if (hp_read_data(d_params, d_out, pstream, sq.contextInfo.ubJfs.headAddr, 4, u32buf) == 0)
			head0 = *(uint32_t *)u32buf;
		uint8_t wqe[64];
		memset(wqe, 0, sizeof(wqe));
		struct n2_jfs_sqe_ctl *sqc = (struct n2_jfs_sqe_ctl *)wqe;
		struct n2_wqe_sge *sge = (struct n2_wqe_sge *)(wqe + 48);
		sqc->sqe_bb_idx = (uint16_t)((head0 + 1) & 0xFFFF);
		sqc->flag = 0x20;
		sqc->token_en = 1;
		sqc->rmt_jetty_type = 1;
		sqc->owner = 1;
		sqc->opcode = 0;			/* SEND（PTO UrmaOpcode::SEND，urma_types.hpp:70） */
		sqc->tp_id = sq.contextInfo.ubJfs.tpID;
		sqc->sge_num = 1;
		sqc->rmt_jetty_or_seg_id = rtid & 0xFFFFFu;
		memcpy(sqc->rmt_eid, sq.contextInfo.ubJfs.remoteEID, 16);
		sqc->rmt_token_value = rtval;
		sge->length = 96;
		sge->token_id = ltid;
		sge->va = lva;
		printf("  sendsq：SEND WQE 就绪（opcode=0 len=96 imm=未用/生产口径 imm_data=0）→ 发射\n");
		int frc = hp_fire(d_params, d_out, pstream,
				  sq.contextInfo.ubJfs.sqVa + (uint64_t)(head0 % sq.contextInfo.ubJfs.sqDepth) * 64u,
				  sq.contextInfo.ubJfs.dbVa, sq.contextInfo.ubJfs.headAddr, head0 + 1, wqe);
		printf("  sendsq：hp_fire rc=%d（SQ 槽位=0x%llx head=%u→%u）\n", frc,
		       (unsigned long long)(sq.contextInfo.ubJfs.sqVa + (uint64_t)(head0 % sq.contextInfo.ubJfs.sqDepth) * 64u),
		       head0, head0 + 1);
		/* 3) 收割发送侧 CQE（owner-bit，12-c 形态） */
		int got = 0;
		uint32_t cqe_dw0 = 0;
		for (int w = 0; w < 5000; w++) {
			uint8_t pb[4] = {0};
			if (hp_read_data(d_params, d_out, pstream, cq.contextInfo.ubJfc.scqVa, 4, pb) == 0 &&
			    (((*(uint32_t *)pb) >> 2) & 1) == 1) {
				cqe_dw0 = *(uint32_t *)pb;
				got = 1;
				break;
			}
			usleep(1000);
		}
		if (got) {
			uint8_t cqeraw[32];
			memset(cqeraw, 0, sizeof(cqeraw));
			hp_read_data(d_params, d_out, pstream, cq.contextInfo.ubJfc.scqVa, 32, cqeraw);
			struct n2_jfc_cqe *cqe = (struct n2_jfc_cqe *)cqeraw;
			printf("  sendsq：发送侧 CQE status=%u substatus=%u owner=%u（scqVa=0x%llx）\n",
			       cqe->status, cqe->substatus, cqe->owner,
			       (unsigned long long)cq.contextInfo.ubJfc.scqVa);
		} else {
			printf("  sendsq：发送侧 CQE 超时未见（如实记录）\n");
		}
		/* 4) 轮询 cqe_buf[0] valid（stock 语义：TGT RDMA 直写 + padding 置位，host 轮询——
		 *    nvme_hcomm.c:139-149 在案锚点；本机 dbuf 为 SVM 映射，host 直解引用 core dump
		 *    在案，轮询一律 aclrtMemcpy D2H） */
		int cqe_ok = 0;
		for (int w = 0; w < 15000; w++) {
			uint8_t vb[32];
			if (aclrtMemcpy(vb, sizeof(vb), (char *)dbuf + 0x100, sizeof(vb),
					ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS)
				break;
			if (vb[16] == 1) {
				cqe_ok = 1;
				printf("  sendsq：cqe_buf[0] valid 置位（cqe_va=0x%llx）：cpl cid=%u status=%u\n",
				       (unsigned long long)cqe_va,
				       (uint32_t)(vb[0] | (vb[1] << 8)),
				       (uint32_t)(vb[12] | (vb[13] << 8)));
				break;
			}
			usleep(1000);
		}
		/* 5) 数据比对（256B 递增图案） */
		if (cqe_ok) {
			uint32_t gotd[64];
			int dmatch = 1;
			aclrtMemcpy(gotd, sizeof(gotd), (char *)dbuf + 0x200, sizeof(gotd), ACL_MEMCPY_DEVICE_TO_HOST);
			for (int i = 0; i < 64; i++)
				if (gotd[i] != (uint32_t)i) { dmatch = 0; break; }
			printf("  sendsq：TGT 回写数据 256B @0x%llx 比对=%s（fnv 首字=0x%x）\n",
			       (unsigned long long)data_va, dmatch ? "MATCH" : "MISMATCH", gotd[0]);
			printf("  [%s] sendsq 闭环：SEND 载荷→对端执行→cqe_buf 回执→数据 MATCH\n", dmatch ? "PASS" : "FAIL");
		} else {
			printf("  [FAIL] sendsq 闭环：cqe_buf[0] valid 超时未置位（如实记录）\n");
		}
		touch("/tmp/hcomm_fire_done_lijiawen");
	}

	/* ================= 任务 12-c：AIV 物理发射（fire_tid!=0 侧执行） ================= */
	if (fire_tid != 0) {
		int fire_rc = 0;
		/* 0xFFFFFFFF = 用 remoteBuf 实发 tokenId（合法发射）；其余为字面值（非法对照） */
		uint32_t tid_use = (fire_tid == 0xFFFFFFFFu) ?
			rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId : fire_tid;
		/* head/tail 影子（0x120… 区）host 直读 core dump 实证不可达——
		 * 一律经 AIV 进程内探针读（12-c 第二轮修正） */
		uint8_t u32buf[4] = {0};
		uint32_t head0 = 0;
		if (hp_read_data(d_params, d_out, pstream, sq.contextInfo.ubJfs.headAddr, 4, u32buf) == 0)
			head0 = *(uint32_t *)u32buf;
		else
			printf("WARN head 影子 AIV 读失败，按 0 处理（如实记录）\n");
		printf("=== 12-c 发射开始：fire_tid=0x%x（实发 tid=0x%x）head0=%u ===\n", fire_tid, tid_use, head0);
		fire_tid = tid_use;

		/* 阶段 0：dbVa 无害写（写 0 = 无新工作；14-a 遗留写面首验） */
		int s0 = hp_harmless_db_write(d_params, d_out, pstream, sq.contextInfo.ubJfs.dbVa);
		int32_t st2 = -1;
		HcommChannelGetStatus(&chan, 1, &st2);
		printf("stage0 dbVa 无害写 rc=%d，写后 GetStatus=%d（%s）\n", s0, (int)st2,
		       (s0 == 0 && st2 == HCOMM_CHANNEL_STATUS_READY) ? "写面可达" : "写面异常");
		if (s0 != 0 || st2 != HCOMM_CHANNEL_STATUS_READY) {
			printf("分支 D：dbVa 写面阻断（读面已证可达，写面单独定位）\n");
			fire_rc = 4;
		} else {

		/* 阶段 A：WQE 组装（64B WQEBB；布局 n2_jfs_sqe_ctl/n2_wqe_sge） */
		uint8_t wqe[64];
		memset(wqe, 0, sizeof(wqe));
		struct n2_jfs_sqe_ctl *sqe = (struct n2_jfs_sqe_ctl *)wqe;
		struct n2_wqe_sge *sge = (struct n2_wqe_sge *)(wqe + 48);
		sqe->sqe_bb_idx = (uint16_t)((head0 + 1) & 0xFFFF);
		sqe->flag = 0x20;			/* CQE（12-b ground truth 同值） */
		sqe->token_en = 1;			/* hcomm 档位（tokenValueValid=true，PTO FillSqeCtx） */
		sqe->rmt_jetty_type = 1;
		sqe->owner = 1;				/* head0=0 → owner=1（provider 口径） */
		sqe->target_hint = 0;
		sqe->opcode = 6;			/* READ */
		sqe->tp_id = sq.contextInfo.ubJfs.tpID;
		sqe->sge_num = 1;
		sqe->rmt_jetty_or_seg_id = fire_tid & 0xFFFFFu;
		memcpy(sqe->rmt_eid, sq.contextInfo.ubJfs.remoteEID, 16);
		sqe->rmt_token_value = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue;
		sqe->rmt_addr_l_or_token_id = (uint32_t)(rb.bufferInfo.rma.addr & 0xFFFFFFFFull);
		sqe->rmt_addr_h_or_token_value = (uint32_t)(rb.bufferInfo.rma.addr >> 32);
		sge->length = 256;
		sge->token_id = lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;	/* PTO localTokenId 口径 */
		sge->va = (uint64_t)(uintptr_t)dbuf;
		printf("WQE: tid=0x%x tokenValue=0x%x tpID=0x%x rmtAddr=0x%llx localTokenId=%u localVa=0x%llx\n",
		       fire_tid, sqe->rmt_token_value, sqe->tp_id,
		       (unsigned long long)rb.bufferInfo.rma.addr, sge->token_id,
		       (unsigned long long)sge->va);

		/* 落区毒药 + CQ tail 初值（aclrtMemset——host 直写不可达同上） */
		if (aclrtMemset(dbuf, 256, 0xEE, 256) != ACL_SUCCESS) {
			printf("FAIL 落区毒药\n");
			fire_rc = 4;
		} else {
		uint8_t tbuf[4] = {0};
		uint32_t cq_tail0 = 0;
		if (hp_read_data(d_params, d_out, pstream, cq.contextInfo.ubJfc.tailAddr, 4, tbuf) == 0)
			cq_tail0 = *(uint32_t *)tbuf;

		/* 发射 */
		int frc = hp_fire(d_params, d_out, pstream,
				  sq.contextInfo.ubJfs.sqVa + (uint64_t)(head0 % sq.contextInfo.ubJfs.sqDepth) * 64,
				  sq.contextInfo.ubJfs.dbVa, sq.contextInfo.ubJfs.headAddr, head0 + 1, wqe);
		printf("hp_fire rc=%d\n", frc);
		if (frc != 0) {
			printf("分支 D：AIV 提交步异常 rc=%d\n", frc);
			fire_rc = 4;
		} else {

		/* CQ owner-bit 轮询（PTO WaitForUrmaCqe 形态：环内 owner 翻转——
		 * tail 影子为 SW 发布、HW 不更新（首轮实证：CQE 已在环内 status=0
		 * 而影子恒 0），host 循环 + AIV 单点读，~15s 上限） */
		int arrived = 0;
		for (int i = 0; i < 5000; i++) {
			uint8_t pb[4] = {0};
			if (hp_read_data(d_params, d_out, pstream, cq.contextInfo.ubJfc.scqVa, 4, pb) == 0 &&
			    (((*(uint32_t *)pb) >> 2) & 1) == 1) {
				arrived = 1;
				break;
			}
			usleep(3000);
		}
		if (!arrived) {
			printf("FIRE tid=0x%x => CQE owner 超时未翻转（无完成，不应答类迹象）\n", fire_tid);
			fire_rc = 3;
		}
		/* 诊断dump（任何结局）：sq/cq 影子 + SQ 槽回读 + CQE 环前 64B——
		 * 区分「门铃未触发消费」与「已消费且 CQE 在环内但影子为 SW 发布」 */
		{
			uint8_t dbg[64];
			memset(dbg, 0, sizeof(dbg));
			uint32_t sq_tail = 0xFFFFFFFF, cq_tail = 0xFFFFFFFF;
			if (hp_read_data(d_params, d_out, pstream, sq.contextInfo.ubJfs.tailAddr, 4, dbg) == 0)
				sq_tail = *(uint32_t *)dbg;
			if (hp_read_data(d_params, d_out, pstream, cq.contextInfo.ubJfc.tailAddr, 4, dbg) == 0)
				cq_tail = *(uint32_t *)dbg;
			printf("DIAG: sq_tail影子=%u cq_tail影子=%u（发射前 %u/%u）\n",
			       sq_tail, cq_tail, head0, cq_tail0);
			if (hp_read_data(d_params, d_out, pstream,
					 sq.contextInfo.ubJfs.sqVa + (uint64_t)(head0 % sq.contextInfo.ubJfs.sqDepth) * 64,
					 64, dbg) == 0) {
				printf("DIAG SQ槽回读:");
				for (int i = 0; i < 64; i++) printf("%s%02x", i % 16 == 0 ? "\n  " : " ", dbg[i]);
				printf("\n");
			}
			memset(dbg, 0, sizeof(dbg));
			if (hp_read_data(d_params, d_out, pstream, cq.contextInfo.ubJfc.scqVa, 64, dbg) == 0) {
				printf("DIAG CQE环[0]:");
				for (int i = 0; i < 64; i++) printf("%s%02x", i % 16 == 0 ? "\n  " : " ", dbg[i]);
				printf("\n");
			}
		}
		if (arrived) {

		/* CQE 读回（AIV 读 scqVa——host 不可达区，进程内探针） */
		uint8_t cqe_raw[64];
		memset(cqe_raw, 0, sizeof(cqe_raw));
		int crc = hp_read_data(d_params, d_out, pstream, cq.contextInfo.ubJfc.scqVa, 32, cqe_raw);
		struct n2_jfc_cqe *cqe = (struct n2_jfc_cqe *)cqe_raw;
		uint32_t got[64];
		memset(got, 0, sizeof(got));
		int dmatch = 1;
		if (aclrtMemcpy(got, sizeof(got), dbuf, sizeof(got), ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS)
			dmatch = 0;
		for (uint32_t i = 0; dmatch && i < 64; i++)
			if (got[i] != i) { dmatch = 0; break; }
		printf("FIRE tid=0x%x => cqe_read_rc=%d status=%u substatus=%u byte_cnt=%u owner=%u data_cmp=%s\n",
		       fire_tid, crc, crc == 0 ? cqe->status : 0xFFFF, crc == 0 ? cqe->substatus : 0xFFFF,
		       crc == 0 ? cqe->byte_cnt : 0, crc == 0 ? cqe->owner : 0,
		       dmatch ? "MATCH" : "MISMATCH");
		if (crc != 0 || cqe->status != 0 || !dmatch)
			fire_rc = 1;
		}
		}
		}
		}

		touch("/tmp/hcomm_fire_done_lijiawen");
		printf("=== 12-c 发射结束 fire_rc=%d ===\n", fire_rc);
	}

	/* ================= 任务 23b：AIV 直驱 bench（bench 子命令，client 侧） ================= */
	if (g_bench_iters > 0 && !is_server) {
		uint32_t tid_use = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;
		uint32_t tval = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue;
		uint32_t ltid = lb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;
		uint32_t sqDepth = sq.contextInfo.ubJfs.sqDepth;
		uint32_t cqDepth = cq.contextInfo.ubJfc.cqDepth;
		uint8_t u32buf[4] = {0};
		uint32_t head0 = 0;
		if (hp_read_data(d_params, d_out, pstream, sq.contextInfo.ubJfs.headAddr, 4, u32buf) == 0)
			head0 = *(uint32_t *)u32buf;
		printf("=== 23b bench：AIV 直驱 %d 次 x %uB（head0=%u tid=0x%x sqDepth=%u） ===\n",
		       g_bench_iters, g_bench_xlen, head0, tid_use, sqDepth);
		uint64_t *lat = (uint64_t *)calloc(g_bench_iters, sizeof(uint64_t));
		int ok = 0;
		uint64_t wall0 = hp_now_ns();
		for (int it = 0; it < g_bench_iters; it++) {
			uint32_t head = head0 + (uint32_t)it;
			uint8_t wqe[64];
			memset(wqe, 0, sizeof(wqe));
			struct n2_jfs_sqe_ctl *sqe = (struct n2_jfs_sqe_ctl *)wqe;
			struct n2_wqe_sge *sge = (struct n2_wqe_sge *)(wqe + 48);
			sqe->sqe_bb_idx = (uint16_t)((head + 1) & 0xFFFF);
			sqe->flag = 0x20;
			sqe->token_en = 1;
			sqe->rmt_jetty_type = 1;
			sqe->owner = ((head / sqDepth) % 2 == 0) ? 1 : 0;
			sqe->target_hint = 0;
			sqe->opcode = 6;			/* READ */
			sqe->tp_id = sq.contextInfo.ubJfs.tpID;
			sqe->sge_num = 1;
			sqe->rmt_jetty_or_seg_id = tid_use & 0xFFFFFu;
			memcpy(sqe->rmt_eid, sq.contextInfo.ubJfs.remoteEID, 16);
			sqe->rmt_token_value = tval;
			sqe->rmt_addr_l_or_token_id = (uint32_t)(rb.bufferInfo.rma.addr & 0xFFFFFFFFull);
			sqe->rmt_addr_h_or_token_value = (uint32_t)(rb.bufferInfo.rma.addr >> 32);
			sge->length = g_bench_xlen;
			sge->token_id = ltid;
			sge->va = (uint64_t)(uintptr_t)dbuf;

			uint64_t cq_slot = cq.contextInfo.ubJfc.scqVa + (uint64_t)((head0 + (uint32_t)it) % cqDepth) * 64u;
			uint32_t exp_owner = (((head0 + (uint32_t)it) / cqDepth) % 2 == 0) ? 1u : 0u;
			uint64_t t0 = hp_now_ns();
			int frc = hp_fire(d_params, d_out, pstream,
					  sq.contextInfo.ubJfs.sqVa + (uint64_t)(head % sqDepth) * 64u,
					  sq.contextInfo.ubJfs.dbVa, sq.contextInfo.ubJfs.headAddr, head + 1, wqe);
			if (frc != 0) { printf("  [bench] 第 %d 次发射 rc=%d\n", it, frc); continue; }
			int arrived = 0;
			for (int w = 0; w < 5000; w++) {
				uint8_t pb[4] = {0};
				if (hp_read_data(d_params, d_out, pstream, cq_slot, 4, pb) == 0 &&
				    (((*(uint32_t *)pb) >> 2) & 1u) == exp_owner) { arrived = 1; break; }
				usleep(1000);
			}
			uint64_t t1 = hp_now_ns();
			if (!arrived) { printf("  [bench] 第 %d 次 CQE 超时\n", it); continue; }
			lat[it] = t1 - t0;
			ok++;
		}
		uint64_t wall1 = hp_now_ns();
		/* 数据校验（最后一次读落区；对端图案段实长 4KB） */
		uint32_t vlen = g_bench_xlen < 4096 ? g_bench_xlen : 4096;
		uint32_t *got = (uint32_t *)malloc(vlen);
		int dmatch = 1;
		if (aclrtMemcpy(got, vlen, dbuf, vlen, ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS)
			dmatch = 0;
		for (uint32_t i = 0; dmatch && i < vlen / 4; i++)
			if (got[i] != i) { dmatch = 0; break; }
		free(got);
		/* 统计（插入排序，iters 小） */
		for (int i = 1; i < ok; i++) {
			uint64_t v = lat[i]; int j = i - 1;
			while (j >= 0 && lat[j] > v) { lat[j + 1] = lat[j]; j--; }
			lat[j + 1] = v;
		}
		uint64_t sum = 0;
		for (int i = 0; i < ok; i++) sum += lat[i];
		if (ok > 0) {
			printf("[bench] n=%d xlen=%u P50=%lluns P99=%lluns avg=%lluns wall=%llums\n",
			       ok, g_bench_xlen,
			       (unsigned long long)lat[ok / 2],
			       (unsigned long long)lat[ok - 1],
			       (unsigned long long)(sum / (uint64_t)ok),
			       (unsigned long long)((wall1 - wall0) / 1000000ull));
			printf("[bench] 吞吐=%.2f IOPS / %.2f MiB/s（host 墙钟窗口）\n",
			       (double)ok * 1e9 / (double)(wall1 - wall0),
			       (double)ok * (double)g_bench_xlen / 1048576.0 * 1e9 / (double)(wall1 - wall0));
		}
		printf("=== 23b bench：%s（ok=%d/%d data=%s） ===\n",
		       (ok == g_bench_iters && dmatch) ? "PASS" : "FAIL",
		       ok, g_bench_iters, dmatch ? "MATCH" : "MISMATCH");
		free(lat);
		touch("/tmp/hcomm_fire_done_lijiawen");
	}
	if (g_bench_iters == 0 || is_server) {
		for (int i = 0; i < 1800; i++) {
			if (access(STOPF, F_OK) == 0)
				break;
			sleep(1);
		}
	}
	HcommChannelDestroy(&chan, 1);
	HcommMemUnreg(ep, mh);
	HcommEndpointDestroy(ep);
	printf("=== %s 退出 ===\n", is_server ? "server" : "client");
	return 0;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc >= 2 && !strcmp(argv[1], "descs")) {
		/* 驱动预置 endpoint 枚举（HcommEndpointGetDescNum/GetDescs，
		 * hcomm_res.h:25-42）——用于对照手工构造 desc 的差异 */
		int acl_dev = argc > 2 ? atoi(argv[2]) : 0;
		if (aclInit(NULL) != ACL_SUCCESS) { printf("FAIL aclInit\n"); return 2; }
		if (aclrtSetDevice(acl_dev) != ACL_SUCCESS) { printf("FAIL SetDevice\n"); return 2; }
		uint32_t n = 0;
		HcommResult hr = HcommEndpointGetDescNum(acl_dev, &n);
		printf("GetDescNum ret=%d num=%u\n", (int)hr, n);
		if (hr == 0 && n > 0) {
			EndpointDesc *ds = (EndpointDesc *)calloc(n, sizeof(EndpointDesc));
			uint32_t cap = n;
			hr = HcommEndpointGetDescs(acl_dev, &cap, ds);
			printf("GetDescs ret=%d got=%u\n", (int)hr, cap);
			for (uint32_t i = 0; i < cap && hr == 0; i++) {
				printf("desc[%u]: protocol=%d addrType=%d eid=%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
				       "%02x%02x:%02x%02x:%02x%02x:%02x%02x loc=%d phy=%u sdid=%u srv=%u pod=%u\n",
				       i, (int)ds[i].protocol, (int)ds[i].commAddr.type,
				       ds[i].commAddr.eid[0], ds[i].commAddr.eid[1], ds[i].commAddr.eid[2], ds[i].commAddr.eid[3],
				       ds[i].commAddr.eid[4], ds[i].commAddr.eid[5], ds[i].commAddr.eid[6], ds[i].commAddr.eid[7],
				       ds[i].commAddr.eid[8], ds[i].commAddr.eid[9], ds[i].commAddr.eid[10], ds[i].commAddr.eid[11],
				       ds[i].commAddr.eid[12], ds[i].commAddr.eid[13], ds[i].commAddr.eid[14], ds[i].commAddr.eid[15],
				       (int)ds[i].loc.locType, ds[i].loc.device.devPhyId, ds[i].loc.device.superDevId,
				       ds[i].loc.device.serverIdx, ds[i].loc.device.superPodIdx);
			}
			free(ds);
		}
		return 0;
	}
	if (argc < 5) {
		printf("usage: %s server|client <aclDev> <urmaDevIdx> <devPhyId> [fire_tid_hex]\n", argv[0]);
		return 64;
	}
	int is_server = !strcmp(argv[1], "server");
	if (!strcmp(argv[1], "wnfire")) {
		g_wnfire = 1;
		g_wniters = argc > 5 ? atoi(argv[5]) : 1;
		return run_side(0, atoi(argv[2]), atoi(argv[3]), (uint32_t)strtoul(argv[4], NULL, 0), 0, 4);
	}
	if (!strcmp(argv[1], "sendsq")) {
		g_sendsq = 1;
		return run_side(0, atoi(argv[2]), atoi(argv[3]), (uint32_t)strtoul(argv[4], NULL, 0), 0, 4);
	}
	if (!strcmp(argv[1], "bench")) {
		/* 23b：bench <aclDev> <urmaDevIdx> <devPhyId> <iters> [xlen]（client 角色，AIV 引擎） */
		g_bench_iters = argc > 5 ? atoi(argv[5]) : 8;
		g_bench_xlen = argc > 6 ? (uint32_t)atol(argv[6]) : 4096;
		return run_side(0, atoi(argv[2]), atoi(argv[3]), (uint32_t)strtoul(argv[4], NULL, 0), 0, 4);
	}
	if (!is_server && strcmp(argv[1], "client") != 0)
		return 64;
	uint32_t fire_tid = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 16) : 0;
	int engine_arg = argc > 6 ? atoi(argv[6]) : 4;	/* 4=COMM_ENGINE_AIV（默认不变） */
	return run_side(is_server, atoi(argv[2]), atoi(argv[3]), (uint32_t)strtoul(argv[4], NULL, 0), fire_tid, engine_arg);
}
