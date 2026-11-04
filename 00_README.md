# ano_offload_pkg v5 — ANO 现役实装形态（真 TGT 迁移作战包）

**ANO = AIV NVMe-oF Offload**。本包只收**唯一现役形态**（任务 43 全链一体全绿基线）：AIV kernel 单次启动完成"读表→组 SQE（生产镜像构造器）→组 128B 双块 WQE→上环敲门→CQ 收割→回执消费+清位"，**发射热路径 host 零指令（prep 一次性物化除外）**。过渡形态（ano_wn 运输面/hov_wn 手拼/hov_send SEND 封存/ano_mq 未验证多队列）一律不在包内——历史全量在 ano_archive 归档包（回滚用）。

**唯一例外声明**：软 TGT（tgt_wn）源出 ano_wn 树，但属在役件（非过渡形态）——新环境先跑软 TGT 自验环境干净性，再动真 TGT（M44 单变量纪律前置）。

## 布局

```
00_README.md / MANIFEST.md5 / DEPENDENCIES.md
src/ano_full/    wn_full.cpp（发射侧源）
src/tgt_wn/      tgt_wn.cpp（软 TGT 源）
src/aiv_probe/   AnoProbe 算子工程（含组帧链 sqe96_build 四件 #include 链）
src/include/     n2_udma_internal.h（UB 线格式布局常量，两端共用唯一出处）
bin/             wn_full / tgt_wn（预编译二进制，环境一致可跳过编译）
vendor/          AnoProbe 算子安装态（customize 全目录）
scripts/         env.sh / build.sh / run_direct.sh / full_pair.sh / deploy_check.sh
docs_inbox/      文档三席（待 Govern 投入，见 PLACEHOLDER.md）
```

## 三句话上手

```bash
md5sum -c MANIFEST.md5                      # ① 完整性校验（应全 OK）
TASK_DEVICE=<卡号> bash scripts/deploy_check.sh   # ② 环境自检（含 dry-run 建链，零发射）
TASK_DEVICE=<卡号> bash scripts/run_direct.sh 1   # ③ 单发联跑（8=八连发）
```

**全绿判据**：`[PASS] wnfire 全链一体闭环（N/N 发 CQE=0 + N/N 发回执 AIV 侧 MATCH）`；对端明细 `/tmp/wn_tgt_log_lijiawen.txt`（key 校验 MATCH×N、keyed word type=0x40）。

**设备号**：本包不预设卡号白名单——新环境必须人工确认健康空闲卡后以 `TASK_DEVICE` 传入。

## 注意

- 预编译二进制的运行前提=目标机 CANN 9.3.0+驱动基线一致；不符则 `bash scripts/build.sh` 重编译（编译命令在案封装），重编译产物 md5 与预编译件不同属正常（包内路径嵌入），判据看行为不看字节；
- AnoProbe vendor 不要现场重建（需完整算子开发链）；重编译算子仅当改动 ano_probe.cpp 时，路径：`cd src/aiv_probe && bash build.sh` 后安装 `.run`（`--install-path` 须绝对路径）；
- 更多：机制原理见归档包内算子文档；缝合点（真 TGT 对接两处替换）以源码注释与任务 41 缝合点文档（归档包）为准。
