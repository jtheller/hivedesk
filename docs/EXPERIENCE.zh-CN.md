# 画面与输入测量（0.4.0）

编码仍是 HEVC 4:4:4、原有 P1 / 无 B 帧 / 无 lookahead / 自动 CBR。本轮不通过改变编码质量、显示模式或增加播放缓冲来换取平滑曲线。

## 采集

开启设置中的 Debug overlay 后，正常使用即可。每台观看端在自己的 LocalAppData/HiveDesk 中生成 `experience-进程号.csv`。每个文件最多约 64 MiB，再滚动到 `.previous.csv`；新进程保留最多四个旧文件（正在被其他进程使用而不能删除的文件除外）。关闭 Debug 后不再产生新记录。

队列固定 8192 条，生产线程只尝试入队；竞争或队列满时丢弃诊断记录并计数，绝不等待磁盘。独立线程批量写入。记录不含键值、鼠标坐标、剪贴板或图像。这里的日志仍有一定测量开销，可用 Debug 开/关作对照。

将不同电脑的日志复制到不同目录，再运行：

```powershell
python scripts/analyze-experience.py '.local/capture/DEVG/experience-*.csv'
python scripts/analyze-experience.py '.local/capture/VI/experience-*.csv' --session 123456
```

不要筛掉低 FPS 的记录。每个方向至少记录连续拖窗/滚动、大转场及静止段的起止时间；启动、预热停留和静止画面的长间隔不能算成持续运动卡顿。分析器给的是实际事件样本分位数，不是窗口 P95 的 P95。

## 指标边界

- 源端 capture-to-ready 耗时来自同一台机器的时钟；不与观看端绝对时间相减。
- Arrival → Prepared → Present 分开计算接收后解码转换、准备后等待、总接收后驻留。
- Present 是成功提交。额外记录 DXGI 的 PresentCount、PresentRefreshCount、SyncRefreshCount、SyncQPCTime、HRESULT 和 GetLastPresentCount。只对连续观测的已显示编号计算刷新周期间隔，漏观测单列，不能作为丢帧数。
- DXGI 统计可能不可用或延迟，特别是硬件显示队列、多屏/其他全屏应用。此时保持 unavailable；不调用 DwmFlush 强行同步，以免改变待测管线。仍需 PresentMon/ETW 或外部测量补充验证。
- 输入发送和完成 ACK 按 session + sequence 对齐；ACK 包含注入和返回网络，不是应用响应或物理 input-to-photon。
- diagnostics.json/jsonl 增加输入 credit_stalls、credit_wait_total_ms、credit_wait_max_ms，判断固定八包在途额度是否真的阻塞。未凭猜测加大输入队列。
- 240Hz 固定刷新显示 100 FPS 内容可能有 2/3 个刷新周期的重复帧节奏。它与几十/上百毫秒的停顿是不同问题，不能混为一谈。

最终“USB 手感”验收需要同一输入设备、同一应用与显示条件下的本地/远程对照，必要时用高速摄影或光电测量。软件 ACK 不能代替这个结论。

## 预热

Hive 当前可见、在线且支持预热的所有远端立即进入预热集合，滚动时更新；初始化在后台受控串行执行，收到选择任务时让位。每台远端先创建实际捕获/NVENC，编码一张画面后暂停产帧；本机创建隐藏 HWND、NVDEC 和渲染资源，解码这张画面。点击后复用同一会话和 HWND，恢复编码并要求新 IDR，随后完成剪贴板与输入交接。

预热不接管控制权、不写剪贴板、不启用网络输入。已被控制或已被另一台机器预热的目标可拒绝预热，点击仍走正常连接。取消带 session；已提升为主动连接的会话不会被旧预热取消请求终止。可见期间每两秒续租；离开可视集合后保留约两秒，远端失联租约十秒过期。资源保留期仍占显存，但不持续编码全部机器。

双方需升级到 0.4.0 才能使用预热；旧版本保留正常冷连接。首次点击发生在预热完成前时，仍可能等待，不能承诺零延迟。真实画面预热也不是实时直播。

安装后仅在没有活动连接时，可显式运行 `HiveDeskCheck.exe --warm` 检查 SYSTEM broker 的首帧、暂停、提升确认和新 IDR。它不安装输入钩子、不发送任何键鼠事件；不属于默认检查。

参考：[DXGI GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics)。
