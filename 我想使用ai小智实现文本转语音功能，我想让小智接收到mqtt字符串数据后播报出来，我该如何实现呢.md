我想使用ai小智实现文本转语音功能，我想让小智接收到mqtt字符串数据后播报出来，我该如何实现呢
esp32-s3-DevkitC-1 N16R8

问题记录:
可以实现屏幕显示文本，但是无法语音播报
关机后mqtt连接还在？



实际：

```shell
I (118081) TtsSpeaker: Received message on topic xiaozhi/tts/message: 你好
W (118081) TtsSpeaker: 设备忙，将消息加入队列。当前状态: 5
I (118081) TtsSpeaker: 消息已加入队列，当前队列长度: 1

I (222431) TtsSpeaker: 从队列播放消息: 你好 (剩余 2 条消息)
W (222431) Application: Alert TTS消息: 你好 [happy]
I (223431) TtsSpeaker: 从队列播放消息: 你好呀 (剩余 1 条消息)
W (223431) Application: Alert TTS消息: 你好呀 [happy]
I (224431) TtsSpeaker: 从队列播放消息: 你好呀哈哈哈 (剩余 0 条消息)
W (224431) Application: Alert TTS消息: 你好呀哈哈哈 [happy]
I (231231) Application: Free internal: 16155 minimal internal: 11719

I (269991) TtsSpeaker: Received message on topic xiaozhi/tts/message: 你好呀哈哈哈
I (269991) TtsSpeaker: Playing TTS message: 你好呀哈哈哈
W (270001) Application: Alert TTS消息: 你好呀哈哈哈 [happy]
I (271231) Application: Free internal: 16139 minimal internal: 11719
I (297361) Application: STATE: speaking
I (297361) Application: >> 播报文本。
I (297721) Application: << 好的，你想让我播报什么内容呢？
I (300721) Application: STATE: listening
I (303861) Application: STATE: speaking
I (303871) Application: >> 播报刚刚接受的内容。
I (304261) Application: << 好的，我来播报一下刚刚的内容：“播报刚刚接受的内容。
W (304521) MQTT: Received audio packet with wrong sequence: 116, expected: 114
W (304521) MQTT: Received audio packet with old sequence: 114, expected: 116
W (304521) MQTT: Received audio packet with old sequence: 115, expected: 116
I (308441) Application: << ”

如果你有其他内容需要播报，告诉我哦。
```

测试2、3会响





方案

回顾 xiaozhi-esp32 (来自您提供的GitHub链接 https://github.com/78/xiaozhi-esp32) 的常规TTS流程（基于其设计）： 

 通常这类项目的TTS流程是： 

 用户通过按键或语音唤醒词激活设备。 

 设备进入聆听状态 (kDeviceStateListening)。 

 用户说出语音指令。 

 Application 将捕获到的语音数据通过 Protocol 发送到云端进行ASR（语音识别）。 

 ASR结果（文本）返回给 Application。 

 Application 将ASR文本发送给LLM（大语言模型）。 

 LLM返回响应文本。 

 Application 将LLM的响应文本通过 Protocol 发送到云端进行TTS（文本转语音）。 

 TTS服务返回音频流。 

 Application 播放音频流，此时设备状态变为 kDeviceStateSpeaking。 

 播放完毕，设备返回 kDeviceStateIdle。 

 我们的 TtsSpeaker 尝试做的事情： 

 我们试图通过 SendTts 函数直接注入文本到这个流程的第8步（或类似的地方），期望 Application 能直接将我们的文本发送给TTS服务器。 

 当前的问题根源很可能在于： 

 Application 类并没有提供一个直接的API来接收一个外部文本字符串并启动TTS流程。它主要依赖于语音输入和内部状态机来驱动整个对话和TTS过程。我们通过 display->SetChatMessage() 和 app.ToggleChatState() 的组合拳，未能成功“欺骗”或“触发” Application 按照我们的意图执行TTS。 



目的：
1.在小智与主人对话的过程中，如果接受到mqt消息则按顺序储存，等小智回到待机时自动播报文本内容。

2.

请分析以上内容，给我解决方案。



先检查能不能上传服务器、能不能接受到服务器的反馈，再检查能不能正常播报



问题一：开机后接收到mqtt消息后，能够进行语音播报，但是会触发ASR语音识别功能，请帮我设置在使用

问题二：出现以上警告或者错误导致编译失败，请分析和解决问题