# ano_full — ANO 现役主线：AIV 全链一体原型（任务 43 形态）

> ANO = AIV NVMe-oF Offload。本目录是**当前唯一现役代码线**：发射热路径 host 零指令（prep 一次性物化除外）——AIV kernel 单次启动完成"读表→组 SQE→组 WQE→上环敲门→CQ 收割→回执消费+清位"。

## 组成

| 件 | 角色 | md5（任务 43 基线） |
|---|---|---|
| `wn_full.cpp` / `wn_full` | 发射侧（client）：建链→prep 物化→热路径每发 aclnn 启动+sync | `0dfd62e8…` / `3931835e…` |
| `full_pair.sh` | 联跑编排（tgt 后台+wn 前台） | `ecb93ab6…` |
| `../../taskM42/AnoProbeBuild/` | **AnoProbe 算子源**（kernel `ano_probe.cpp` 含 mode4 全链一体；组帧链 sqe96_build.c/h+sqe96_layout.h+aiv_device_compat.h 拷自 taskM6f，md5 对账在案） | kernel `9457e48d…` |
| `../../taskM42/opp_priv_ano/` | AnoProbe vendor 安装态（aclnn 面 `aclnnAnoProbe*`） | libcust_opapi.so `d9c074f2…` |
| `../../taskM42/ano_wn/tgt_wn` | 软 TGT（对端扮演，**封存件零改动复用**） | `81b6ac50…` |

## 依赖

- aarch64 + CANN 9.3.0（`ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.3.0`）+ 驱动 25.6.rc2.b023／固件 7.0.9.100.202（实测面）
- 链接：`-lhcomm -lascendcl -lruntime -lc_sec -lurma -lcust_opapi -lnnopbase`
- 运行时环境（full_pair.sh 已封装）：`ASCEND_CUSTOM_OPP_PATH=opp_priv_ano/vendors/customize`、`ASCEND_OPP_PATH=opp_merged_ano`（symlink 农场：系统 opp+包内 custom）

## 构建

```bash
# 算子（改了 ano_probe.cpp 才需要）
cd ~/aiv_scratch/taskM42/AnoProbeBuild && export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.3.0 && bash build.sh
./build_out/custom_opp_openEuler_aarch64.run --quiet --install-path=<abs path>/opp_priv_ano
# host 侧
cd ~/aiv_scratch/taskM43/ano_full
C=/usr/local/Ascend/cann-9.3.0; V=~/aiv_scratch/taskM42/opp_priv_ano/vendors/customize; K=~/aiv_scratch/taskM42/AnoProbeBuild/op_kernel
g++ -std=c++17 -O2 -fpermissive -o wn_full wn_full.cpp -I$C/include -I$C/aarch64-linux/include -I/usr/include \
  -I$V/op_api/include -I$K -I$HOME/lijiawen_ws/soft_tgt/n2_probe \
  -L$C/aarch64-linux/lib64 -lhcomm -lascendcl -lruntime -lc_sec -L/usr/lib64 -lurma \
  -L$V/op_api/lib -lcust_opapi -L$C/lib64 -lnnopbase \
  -Wl,-rpath,$C/aarch64-linux/lib64 -Wl,-rpath,/usr/lib64 -Wl,-rpath,$V/op_api/lib -Wl,-rpath,$C/lib64
```

## 运行（本机双卡软 TGT 自证）

```bash
task-submit --timeout 3600 --device 1 --run "bash $PWD/full_pair.sh 1"   # 单发
task-submit --timeout 3600 --device 1 --run "bash $PWD/full_pair.sh 8"   # 八连发
```

**全绿判据**（唯一口径）：
```
[PASS] wnfire 全链一体闭环（N/N 发 CQE=0 + N/N 发回执 AIV 侧 MATCH；热路径零 H2D 代码审查在案）
```
对端明细在 `/tmp/wn_tgt_log_lijiawen.txt`（应有 key 校验 MATCH×N、`type=0x40`、回执 WRITE rc=0×2N）。

## 关键设计（速览）

- **prep↔热路径边界**：prep 一次性 H2D（prep 包 `ano_full_prep` 200B@dbuf+0x600、fire_idx@dbuf+0x500、d_params mode4）；热路径循环体零 H2D——kernel 自 GM 读 fire_idx 推进（cid=fire_idx%8、head=head0+idx×2）。
- **mode4 kernel 链**：读 prep 包 → `sqe96_build()` 组 96B SQE（生产镜像构造器，psdt/type nibble 与 nvme_hcomm.c 一致）→ 组 128B 双块 WQE（任务 36 对拍表）→ slot0 无害门铃→slot1 真门铃 → kernel 内 spin 收割 CQ（dcci+ld_dev 逐轮，HovSm 形态）→ 回执 valid 轮询+256B 比对+清位。
- **回执布局**（client dbuf）：cqe_buf@+0x100（32B，valid@byte16）、数据@+0x200（256B）、载荷 staging@+0x400、fire_idx@+0x500、prep 包@+0x600。

## 纪律

- 修改必须留痕（文件/位置/新旧 md5/理由）；封存件（ano_wn 五件、六件、hov_send）只读；
- device 白名单 1/2（排队不抢）；device 0/3 严禁；不重启；
- 详细交接见 `../REPORT.md`（任务 43）与 `../../taskM44P/ANO_项目全景框架与流程总览.md`。
