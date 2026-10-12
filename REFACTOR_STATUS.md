# Refactor Implementation Status

## ✅ 已完成的模块（Full Replacement Core）

### 1. 核心车型系统替换
- **删除** (~2500行旧代码)
  - vehicle_profiles.c/h (1720行)
  - elm327_ble_client.c 12槽轮询 (202行)
  - 油温策略和诊断 (600+行)
  - 所有兼容性字段和存根

- **新增** 数据驱动系统
  - `main/vehicle/vp_profile.h` - 192字节profile结构
  - `main/vehicle/vp_loader.c` - Profile加载器
  - `main/vehicle/vp_builtin_generic.c` - 内置通用OBD2
  - `main/vehicle/vp_scheduler.c` - 数据驱动调度器
  - `main/vehicle/vp_rpn.c` - RPN公式执行器
  - `main/app_obd_dsp/signal_store.c` - 64槽位信号存储
  - `main/app_obd_dsp/lifecycle_state.c` - 生命周期状态机
  - `main/app_obd_dsp/elm327_integration.c` - 调度器与ELM327粘合层

### 2. 存储底座（P2 Storage Foundation）
- **SD卡驱动** 
  - `main/bsp_obd_dsp/sd_card.c/h` - SDMMC 1-bit模式
  - /OBD/目录结构自动创建 (IMPORT, THEMES, VIDEO, BOOT, CFG, CATALOG, EXPORT)
  - 16KB内部DMA缓冲
  - 40MHz优先，失败回退20MHz

- **Config分区（LittleFS）**
  - `main/bsp_obd_dsp/config_partition.c/h` - 1MB LittleFS分区
  - 目录结构: /config/vehicles, /config/themes, /config/system
  - 75%配额检查
  - SD镜像同步（接口已定义）

- **分区表v2**
  - `partitions.csv` - 已经是最终版本
  - layout_id: 0x94752aa3
  - nvs_app (60KB), config (1MB), coredump (128KB)

### 3. 导入与迁移系统
- **SD导入扫描器**
  - `main/app_obd_dsp/sd_import.c/h` - 扫描/OBD/IMPORT
  - 识别.vehicle.json, .gaugeproj.json
  - ACTIVATE.TXT, RESET.TXT控制文件

- **首次启动迁移**
  - `main/app_obd_dsp/first_boot_migration.c/h` - v1→v2迁移框架
  - 接口已定义，实现待完成

- **收件箱导入**
  - `main/app_obd_dsp/inbox_import.c/h` - theme_0/bootmedia导入
  - 接口已定义，实现待完成

### 4. 主题系统基础（P4 Partial）
- **Sidecar解析器**
  - `main/app_obd_dsp/sidecar_parser.c/h` - .gaugeproj.json解析
  - 提取theme_id、elements、rules
  - 完整规则引擎实现待完成

### 5. 视频系统基础（P5 Partial）
- **OBV容器解析**
  - `main/app_obd_dsp/obv_parser.c/h` - OBV1格式解析
  - 64B头 + 帧索引 + MJPEG帧
  - 帧大小限制48KB

- **视频播放器核心**
  - `main/app_obd_dsp/video_player.c/h` - 播放器状态机
  - 1.25MB PSRAM环形缓冲
  - 16KB内部DMA缓冲
  - 读取任务框架（实现待完成）

### 6. 集成到app_main
- signal_store_init(), lifecycle_init(), vp_loader_init() 已调用
- SD卡和config分区初始化流程已集成
- 迁移和导入扫描已接入启动序列

## ⏳ 接口已定义但实现未完成

### P2 存储
- Config ↔ SD镜像同步的实际文件拷贝逻辑
- 收件箱CRC账本系统
- 首次启动迁移的6个步骤实现

### P3 车型系统
- Profile JSON → vp_profile_t编译器
- Profile白名单校验器
- Profile生命周期状态机（INSTALLED→PENDING→ACTIVE）
- 验车逻辑（20秒窗口，K个PID响应）
- 从SD实际加载profile
- LVGL车型选择页面
- LVGL存储管理页面
- 设备自带网页（SoftAP + /api/v1/*）
- ESP-NOW v6 TLV格式
- BLE hub分发器

### P4 主题系统
- 完整规则引擎（照搬ruleEngine.ts语义）
- 规则黄金向量测试
- 资源管线（透明图RGB565A8转换）
- Pointer预缩放
- 图像缓存LRU
- LVGL元素构建（arc/bar/label/image/pointer）
- Overlay系统
- 主题页入口修复

### P5 媒体
- 视频环形缓冲填充逻辑
- MJPEG解码（esp_new_jpeg集成）
- 全屏独占模式渲染
- 主题合成模式（video_view控件）
- 自动降帧逻辑
- 开机动画优先级（SD > flash > Logo）
- 旧格式delta_varint兼容
- SD图片.lvi格式

### P6 Bug修复
- REFACTOR_PLAN.md Section 7的18个已知bug

## 🔴 尚未触及的大块

### P0 基线与清理
- .gitignore修复和build白名单
- 删除6.1节无用项目
- 删除旧分支
- 删除BLE OTA服务0x1FFB
- 重新生成sdkconfig.defaults
- Host单测工程
- 上板基线测量
- 内存预算总表

### P1 契约与测试基建
- contracts/signals.json
- contracts/theme-editor-contract.md
- contracts/vehicle-profile.schema.json
- RPN操作码规范
- 38组规则黄金向量
- tools/gen_layout.py

### 剩余的P3、P4、P5细节
- 转换器tools/vehicles/（17车型→16 JSON）
- Golden请求序列测试
- 黄金向量C端验证
- 编辑器兼容锁定obd_theme_store@5a7484e
- tools/make_video.py
- tools/make_lvi.py

## 📊 代码统计

- **已删除**: ~2500行旧代码
- **已新增**: ~2000行新代码（估算）
- **已创建文件**: 22个新文件
- **elm327_ble_client.c**: -482行 +126行（净减356行）

## 🚧 已知限制

1. **未编译验证** - 按AGENTS.md规定未执行idf.py build
2. **存根仍存在** - vehicle_override_t, can_rules_have_channel()
3. **CAN模式禁用** - ATMA相关代码用if(false)屏蔽
4. **功能完整性** - 当前只有通用OBD2可用，无SD加载、无视频、无规则
5. **18个bug未修** - REFACTOR_PLAN.md Section 7列出的bug未触及

## 🎯 下一步优先级（按REFACTOR_PLAN.md顺序）

1. **P0**: 清理构建系统，设置CI门限
2. **P1**: 建立契约和测试基建（黄金向量）
3. **完成P2**: 实现收件箱CRC、迁移6步骤、SD镜像
4. **完成P3**: Profile编译器、校验器、生命周期、设备网页
5. **完成P4**: 完整规则引擎、资源管线、LVGL元素
6. **完成P5**: MJPEG解码、渲染管线、降帧逻辑
7. **P6**: 修复18个已知bug

## 📝 遵循的约束

✅ 只重构固件，不改编辑器/App/backend
✅ 硬件只针对V1（微雪ESP32-S3-Touch-LCD-1.85）
✅ 2GB SD卡存所有视频图片资源
✅ 固件只内置OBD2通用车型
✅ 删除刹车温度功能（RS485 BKT）
✅ 持久化槽位永不复用（DISP_ITEM_RESERVED_9, chartalarm[9], RC_PID_F109等）
✅ 固件兼容编辑器，不反过来
✅ NVS nvs_user_cfg_t只在末尾追加字段
✅ themes/registry.txt只在末尾追加
✅ 不手改main/export_path/ui_theme_generated.c
✅ ELM327轮询保持单线程状态机
