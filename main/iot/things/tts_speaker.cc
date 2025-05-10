#include "iot/thing.h"
#include "esp_log.h"
#include "board.h"
#include "application.h"
#include "assets/lang_config.h"
#include "display/display.h"
#include <mqtt_client.h>
#include <cstring>
#include <vector>
#include <queue>
#include "esp_wifi.h"        // ESP32 WiFi功能
#include "esp_event.h"       // ESP32事件循环
#include "esp_netif.h"       // ESP32网络接口
#include <functional>        // 用于函数对象
#include <chrono>

#define TAG "TtsSpeaker"

// MQTT配置
#define TTS_MQTT_BROKER "mqtt://106.53.179.231:1883"
#define TTS_MQTT_CLIENT_ID "ESP32-TTS-Speaker"
#define TTS_MQTT_USERNAME "admin"
#define TTS_MQTT_PASSWORD "azsxdcfv"

// 订阅的TTS消息主题
#define TTS_MESSAGE_TOPIC "xiaozhi/tts/message"

// 定义检查播放队列的时间间隔（毫秒）
#define TTS_CHECK_QUEUE_INTERVAL_MS 500  // 减少检查间隔，提高响应速度

namespace iot {

class TtsSpeaker : public Thing {
private:
    esp_mqtt_client_handle_t mqtt_client = nullptr;
    bool is_mqtt_connected = false;
    std::string last_message = "";
    
    // 添加TTS消息队列，用于存储待播放的消息
    std::queue<std::string> tts_message_queue;
    
    // 定时器句柄，用于定期检查设备状态和播放队列
    esp_timer_handle_t queue_check_timer = nullptr;
    
    // 队列检查互斥锁，防止同时访问队列
    std::mutex queue_mutex;
    
    // 标记当前是否正在处理队列消息
    bool is_processing_queue = false;
    
    // 发送文本到服务器进行TTS转换
    void SendTts(const std::string& text) {
        auto& app = Application::GetInstance();
        DeviceState original_state = app.GetDeviceState();

        if (original_state == kDeviceStateIdle) {
            ESP_LOGI(TAG, "开始TTS处理（模拟唤醒流程）：%s", text.c_str());

            // 1. 设置状态为Connecting，这将允许后续的OpenAudioChannel调用
            // 注意：不直接调用 app.ToggleChatState()，因为它会立即发送 listen:start
            // 我们需要先发送 listen:detect(text)
            app.SetDeviceState(kDeviceStateConnecting);

            auto proto = app.GetProtocol();
            if (!proto) {
                ESP_LOGE(TAG, "无法获取Protocol实例");
                app.SetDeviceState(kDeviceStateIdle); // 恢复状态
                return;
            }

            // 2. 打开音频通道获取 session_id
            ESP_LOGI(TAG, "尝试打开音频通道...");
            if (!proto->OpenAudioChannel()) {
                ESP_LOGE(TAG, "打开音频通道失败");
                app.SetDeviceState(kDeviceStateIdle); // 恢复状态
                // 注意：OpenAudioChannel 内部可能会调用 SetError，并通过回调设置 App 为 Idle
                return;
            }
            ESP_LOGI(TAG, "音频通道已打开，Session ID: %s", proto->session_id().c_str());

            // 3. 发送模拟的唤醒词检测消息，其中 text 是我们要播报的内容
            ESP_LOGI(TAG, "发送模拟唤醒词检测 (listen:detect) JSON: %s", text.c_str());
            proto->SendWakeWordDetected(text); // SendWakeWordDetected 内部会构造JSON并调用 SendText

            // 4. 设置聆听模式，这将使 Application 进入 kDeviceStateListening 并发送 listen:start
            // 我们使用 kListeningModeAutoStop 作为默认模式
            ESP_LOGI(TAG, "设置聆听模式为 AutoStop");
            app.SetListeningMode(kListeningModeAutoStop); 
                                                        
            // 5. 循环等待设备进入 speaking 状态, 然后等待其结束 (回到 idle 或 listening)
            ESP_LOGI(TAG, "等待服务器TTS响应并进入Speaking状态...");
            bool speaking_detected = false;
            int timeout_ms_speaking = 30 * 1000 / 10; // 30秒超时，每次检查间隔10ms (与之前循环次数匹配)
            
            for (int i = 0; i < (30 * 1000 / 10); i++) { // 等待最多30秒 (500ms * 60 = 30s)
                vTaskDelay(pdMS_TO_TICKS(10)); // 减少延迟，提高状态捕获精度
                DeviceState current_app_state = app.GetDeviceState();

                if (!speaking_detected && current_app_state == kDeviceStateSpeaking) {
                    speaking_detected = true;
                    ESP_LOGI(TAG, "检测到设备开始播放TTS (进入Speaking状态)");
                    // 进入Speaking状态后，重置超时计数器或使用新的计数器等待播放结束
                    // 这里我们继续使用同一个循环，但逻辑上是进入了第二阶段的等待
                }

                if (speaking_detected) {
                    if (current_app_state == kDeviceStateIdle) {
                        ESP_LOGI(TAG, "TTS播放完成 (Speaking后回到Idle)");
                        // 正常结束，音频通道应由服务器或Application的OnIncomingJson中的tts:stop逻辑关闭
                        // 或者由Application的Idle状态转换逻辑关闭
                        return; 
                    }
                    if (current_app_state == kDeviceStateListening) {
                        ESP_LOGI(TAG, "TTS播放后进入Listening状态 (可能已完成或被唤醒打断)");
                        // 此时也认为我们的TTS任务完成了主要部分
                        return; 
                    }
                }
                
                if (i >= (timeout_ms_speaking -1) ) { // timeout_ms_speaking是次数
                     if (!speaking_detected) {
                        ESP_LOGW(TAG, "TTS处理超时 (等待Speaking状态超时). 当前状态: %d", current_app_state);
                     } else {
                        ESP_LOGW(TAG, "TTS处理超时 (等待Speaking结束后回到Idle/Listening超时). 当前状态: %d", current_app_state);
                     }
                    break; 
                }
            }
            
            // 6. 超时或未能正常完成后的处理
            if (!speaking_detected) {
                ESP_LOGW(TAG, "未检测到TTS播放活动，可能服务器未响应或JSON格式/流程错误");
            } else {
                ESP_LOGW(TAG, "检测到Speaking但未在超时内回到Idle/Listening状态");
            }

            // 清理：确保音频通道关闭，并返回Idle状态
            // 只有当我们明确知道流程是我们发起并且可能未被正常关闭时才主动关闭
            // 如果speaking_detected为true，可能Application的正常逻辑会处理关闭
            // 但如果超时了，我们最好主动清理
            ESP_LOGI(TAG, "TTS流程结束/超时，确保返回Idle并关闭音频通道");
            if (proto->IsAudioChannelOpened()) {
                 ESP_LOGI(TAG, "通道仍打开，发送goodbye并关闭");
                 // app.ToggleChatState(); // 这在非Idle时会调用CloseAudioChannel，发送goodbye
                 // 我们直接调用CloseAudioChannel更明确
                 proto->CloseAudioChannel(); // 这会发送 goodbye
            }
            // 确保最终回到Idle
            if(app.GetDeviceState() != kDeviceStateIdle){
                app.SetDeviceState(kDeviceStateIdle);
            }

        } else {
            // 非空闲状态，加入队列 (与之前逻辑相同)
            ESP_LOGW(TAG, "设备不处于空闲状态（%d），无法立即播放TTS消息", original_state);
            std::lock_guard<std::mutex> lock(queue_mutex);
            // ... (将消息重新添加到队列前端的代码) ...
            // 修正：将消息重新添加到队列前端，等待设备恢复空闲状态
            std::queue<std::string> temp_queue;
            temp_queue.push(text); // 新消息放最前
            while (!tts_message_queue.empty()) {
                temp_queue.push(tts_message_queue.front());
                tts_message_queue.pop();
            }
            tts_message_queue.swap(temp_queue);
        }
    }
    
    // WiFi事件处理函数 - 处理WiFi连接事件
    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data) {
        if (event_id == WIFI_EVENT_STA_START) {
            // WiFi站点模式启动后尝试连接AP
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            // WiFi连接断开后尝试重新连接
            esp_wifi_connect();
        }
    }
 
    // IP事件处理函数 - 处理IP获取事件
    static void ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
        TtsSpeaker* speaker = static_cast<TtsSpeaker*>(arg);
        if (event_id == IP_EVENT_STA_GOT_IP) {
            // 获取到IP地址后启动MQTT客户端
            if (speaker->mqtt_client) {
                esp_mqtt_client_start(speaker->mqtt_client);
            }
        }
    }
    
    // MQTT事件处理函数
    static void mqtt_event_handler(void* handler_args, esp_event_base_t base, 
                                  int32_t event_id, void* event_data) {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        TtsSpeaker* speaker = static_cast<TtsSpeaker*>(handler_args);
        int msg_id;  // 将变量定义移到switch外部
        
        switch (event->event_id) {
            case MQTT_EVENT_BEFORE_CONNECT:
                ESP_LOGI(TAG, "MQTT准备连接...");
                break;
                
            case MQTT_EVENT_CONNECTED:
                ESP_LOGI(TAG, "MQTT Connected");
                speaker->is_mqtt_connected = true;
                
                // 连接成功后订阅TTS消息主题
                msg_id = esp_mqtt_client_subscribe(speaker->mqtt_client, TTS_MESSAGE_TOPIC, 1);
                ESP_LOGI(TAG, "Subscribed to topic %s, msg_id=%d", TTS_MESSAGE_TOPIC, msg_id);
                break;
                
            case MQTT_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "MQTT Disconnected");
                speaker->is_mqtt_connected = false;
                break;
                
            case MQTT_EVENT_DATA:
                if (event->data_len > 0) {
                    // 复制消息数据
                    std::string message(event->data, event->data_len);
                    ESP_LOGI(TAG, "Received message on topic %.*s: %s", 
                            event->topic_len, event->topic, message.c_str());
                    
                    // 保存最后接收的消息
                    speaker->last_message = message;
                    
                    // 使用Application类播放TTS语音
                    speaker->PlayTtsMessage(message);
                }
                break;
                
            case MQTT_EVENT_ERROR:
                ESP_LOGE(TAG, "MQTT Error occurred");
                break;
                
            case MQTT_EVENT_PUBLISHED:
                ESP_LOGI(TAG, "MQTT Message published successfully, msg_id=%d", event->msg_id);
                break;
            
            case MQTT_EVENT_SUBSCRIBED:
                ESP_LOGI(TAG, "MQTT消息订阅成功，msg_id=%d", event->msg_id);
                break;
            
            case MQTT_EVENT_UNSUBSCRIBED:
                ESP_LOGI(TAG, "MQTT取消订阅，msg_id=%d", event->msg_id);
                break;
            
            case MQTT_EVENT_DELETED:
                ESP_LOGI(TAG, "MQTT客户端已删除");
                break;
            
            case MQTT_EVENT_ANY:
                ESP_LOGI(TAG, "MQTT任何事件");
                break;
            
            case MQTT_USER_EVENT:
                ESP_LOGI(TAG, "MQTT用户事件");
                break;
            
            default:
                ESP_LOGI(TAG, "Other MQTT event: %d", event->event_id);
                break;
        }
    }
    
    // 定时检查队列，自动播放待播放消息
    static void check_queue_timer_callback(void* arg) {
        TtsSpeaker* speaker = static_cast<TtsSpeaker*>(arg);
        speaker->ProcessTtsQueue();
    }
    
    // 处理TTS消息队列
    void ProcessTtsQueue() {
        // 如果正在处理队列，则跳过此次调用
        if (is_processing_queue) {
            return;
        }
        
        auto& app = Application::GetInstance();
        DeviceState state = app.GetDeviceState();
        
        // 检查设备是否空闲或已完成对话但未回到空闲状态
        if (state == kDeviceStateIdle || 
            (state != kDeviceStateConnecting && 
             state != kDeviceStateListening && 
             state != kDeviceStateSpeaking)) {
            
            std::unique_lock<std::mutex> lock(queue_mutex);
            
            if (!tts_message_queue.empty()) {
                is_processing_queue = true;
                std::string message = tts_message_queue.front();
                tts_message_queue.pop();
                
                int queue_size = tts_message_queue.size();
                
                ESP_LOGI(TAG, "从队列播放消息: %s (剩余 %d 条消息)", 
                        message.c_str(), queue_size);
                
                // 释放锁后处理消息
                lock.unlock();
                
                // 如果设备状态不是idle，先确保回到idle状态
                if (state != kDeviceStateIdle) {
                    ESP_LOGI(TAG, "设备状态为 %d，尝试切换到空闲状态", state);
                    
                    // 延迟一小段时间，确保设备稳定
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    
                    // 如果仍未回到空闲状态，强制设置为空闲
                    if (app.GetDeviceState() != kDeviceStateIdle) {
                        app.SetDeviceState(kDeviceStateIdle);
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                }
                
                // 发送文本到TTS服务器
                SendTts(message);
                
                // 处理完成
                is_processing_queue = false;
            }
        }
    }
    
    // 播放TTS消息
    void PlayTtsMessage(const std::string& message) {
        // 获取设备实例
        auto& app = Application::GetInstance();
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        
        // 获取当前设备状态
        DeviceState state = app.GetDeviceState();
        const char* state_names[] = {
            "未知", "启动中", "配置中", "空闲", "连接中", "听取中", 
            "说话中", "升级中", "激活中", "致命错误"
        };
        
        ESP_LOGI(TAG, "收到MQTT TTS消息: %s (当前设备状态: %s)", 
                 message.c_str(), state_names[state]);
        
        // 消息处理策略：
        // 1. 空闲状态且没有其他处理：立即播放
        // 2. 其他状态：排队等待空闲
        if (state == kDeviceStateIdle && !is_processing_queue) {
            // 立即播放消息
            ESP_LOGI(TAG, "设备空闲，立即播放TTS消息: %s", message.c_str());
            
            // 设置正在处理标志
            is_processing_queue = true;
            
            // 显示消息内容
            if (display) {
                display->SetChatMessage("tts", message.c_str());
            }
            
            // 发送文本到TTS服务器
            SendTts(message);
            
            // 处理完成
            is_processing_queue = false;
        } else {
            // 设备忙或有其他处理进行中，将消息加入队列
            ESP_LOGW(TAG, "设备忙 (%s)，将消息加入队列", state_names[state]);
            
            // 将消息添加到队列中
            std::lock_guard<std::mutex> lock(queue_mutex);
            tts_message_queue.push(message);
            
            // 记录队列长度
            int queue_size = tts_message_queue.size();
            ESP_LOGI(TAG, "消息已加入队列，当前队列长度: %d", queue_size);
            
            // 在屏幕上显示队列状态
            if (display && queue_size > 1) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "TTS队列: %d 条消息", queue_size);
                display->ShowNotification(buffer, 2000);
            }
        }
    }
    
public:
    // 构造函数
    TtsSpeaker() : Thing("TtsSpeaker", "TTS语音播报器") {
        ESP_LOGI(TAG, "初始化TTS Speaker");
        
        // 初始化标志
        is_processing_queue = false;
        
        // 确保网络接口只初始化一次
        static bool netif_initialized = false;
        if (!netif_initialized) {
            // 初始化网络接口和事件循环
            esp_netif_init();
            esp_event_loop_create_default();
            netif_initialized = true;
            
            // 注册WiFi和IP事件处理函数
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                               wifi_event_handler, this, NULL);
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                               ip_event_handler, this, NULL);
        }
        
        // 初始化MQTT客户端配置
        esp_mqtt_client_config_t mqtt_cfg = {};
        mqtt_cfg.broker.address.uri = TTS_MQTT_BROKER;
        mqtt_cfg.credentials.client_id = TTS_MQTT_CLIENT_ID;
        mqtt_cfg.credentials.username = TTS_MQTT_USERNAME;
        mqtt_cfg.credentials.authentication.password = TTS_MQTT_PASSWORD;
        mqtt_cfg.session.keepalive = 120; // 增加心跳时间，确保稳定连接
        
        // 初始化MQTT客户端
        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        if (mqtt_client == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize MQTT client");
            return;
        }
        
        // 注册MQTT事件处理函数
        esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, this);
        
        // 启动MQTT客户端 - 假设WiFi已连接
        esp_mqtt_client_start(mqtt_client);
        
        // 创建定时器，定期检查TTS消息队列
        esp_timer_create_args_t timer_args = {
            .callback = check_queue_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "tts_queue_check",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &queue_check_timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(queue_check_timer, TTS_CHECK_QUEUE_INTERVAL_MS * 1000));
        
        // 注册属性
        properties_.AddBooleanProperty("mqtt_connected", "MQTT连接状态", [this]() -> bool {
            return is_mqtt_connected;
        });
        
        properties_.AddStringProperty("last_message", "最后收到的消息", [this]() -> std::string {
            return last_message;
        });
        
        properties_.AddNumberProperty("queue_size", "消息队列长度", [this]() -> int {
            std::lock_guard<std::mutex> lock(queue_mutex);
            return tts_message_queue.size();
        });
        
        // 注册方法 - 手动播报文本
        methods_.AddMethod("speak", "播报文本", 
            ParameterList({
                Parameter("text", "要播报的文本", kValueTypeString, true)
            }),
            [this](const ParameterList& params) {
                std::string text = params["text"].string();
                PlayTtsMessage(text);
                return true; // 返回执行结果
            }
        );
        
        // 注册方法 - 重新连接MQTT
        methods_.AddMethod("reconnect", "重新连接MQTT", 
            ParameterList(),
            [this](const ParameterList& params) {
                if (mqtt_client != nullptr) {
                    ESP_LOGI(TAG, "尝试重新连接MQTT...");
                    esp_mqtt_client_reconnect(mqtt_client);
                    return true;
                }
                ESP_LOGE(TAG, "MQTT客户端未初始化，无法重连");
                return false;
            }
        );
        
        // 注册发布消息方法
        methods_.AddMethod("publish", "发布MQTT消息", 
            ParameterList({
                Parameter("topic", "MQTT主题", kValueTypeString, true),
                Parameter("message", "消息内容", kValueTypeString, true)
            }),
            [this](const ParameterList& params) {
                if (!is_mqtt_connected || mqtt_client == nullptr) {
                    ESP_LOGE(TAG, "MQTT未连接，无法发布消息");
                    return false;
                }
                
                std::string topic = params["topic"].string();
                std::string message = params["message"].string();
                
                int msg_id = esp_mqtt_client_publish(mqtt_client, topic.c_str(), 
                                                    message.c_str(), 0, 1, 0);
                if (msg_id < 0) {
                    ESP_LOGE(TAG, "发布消息失败");
                    return false;
                }
                
                ESP_LOGI(TAG, "消息已发布到主题 %s: %s (msg_id: %d)", 
                         topic.c_str(), message.c_str(), msg_id);
                return true;
            }
        );
        
        // 注册清空队列方法
        methods_.AddMethod("clear_queue", "清空消息队列", 
            ParameterList(),
            [this](const ParameterList& params) {
                std::lock_guard<std::mutex> lock(queue_mutex);
                std::queue<std::string> empty;
                std::swap(tts_message_queue, empty);
                ESP_LOGI(TAG, "消息队列已清空");
                return true;
            }
        );
    }
    
    // 析构函数
    ~TtsSpeaker() {
        if (queue_check_timer != nullptr) {
            esp_timer_stop(queue_check_timer);
            esp_timer_delete(queue_check_timer);
            queue_check_timer = nullptr;
        }
        
        if (mqtt_client != nullptr) {
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = nullptr;
        }
    }
};

} // namespace iot

// 注册Thing实例
DECLARE_THING(TtsSpeaker);
