#include "iot/thing.h"
#include "esp_log.h"
#include "board.h"
#include "application.h"
#include "assets/lang_config.h"
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
#define TTS_CHECK_QUEUE_INTERVAL_MS 1000

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
        auto& app = Application::GetInstance();
        
        // 检查设备是否空闲
        if (app.GetDeviceState() == kDeviceStateIdle) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            
            if (!tts_message_queue.empty()) {
                std::string message = tts_message_queue.front();
                tts_message_queue.pop();
                
                ESP_LOGI(TAG, "从队列播放消息: %s (剩余 %d 条消息)", 
                        message.c_str(), tts_message_queue.size());
                
                // 测试不同的声音文件来确定哪个能正常工作
                if (message.find("测试1") != std::string::npos) {
                    ESP_LOGI(TAG, "使用振动音效播放队列消息");
                    app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_VIBRATION);
                } else if (message.find("测试2") != std::string::npos) {
                    ESP_LOGI(TAG, "使用成功音效播放队列消息");
                    app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_SUCCESS);
                } else if (message.find("测试3") != std::string::npos) {
                    ESP_LOGI(TAG, "使用激活音效播放队列消息");
                    app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);
                } else if (message.find("测试4") != std::string::npos) {
                    ESP_LOGI(TAG, "使用感叹音效播放队列消息");
                    app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_EXCLAMATION);
                } else {
                    ESP_LOGI(TAG, "使用感叹音效播放队列消息(默认)");
                    app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_EXCLAMATION);
                }
            }
        }
    }
    
    // 播放TTS消息
    void PlayTtsMessage(const std::string& message) {
        // 使用Application的云端TTS功能
        auto& app = Application::GetInstance();
        
        // 检查设备状态，只有在空闲状态才播放TTS
        if (app.GetDeviceState() == kDeviceStateIdle) {
            ESP_LOGI(TAG, "Playing TTS message: %s", message.c_str());
            
            // 测试不同的声音文件来确定哪个能正常工作
            if (message.find("测试1") != std::string::npos) {
                ESP_LOGI(TAG, "使用振动音效播放直接消息");
                app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_VIBRATION);
            } else if (message.find("测试2") != std::string::npos) {
                ESP_LOGI(TAG, "使用成功音效播放直接消息");
                app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_SUCCESS);
            } else if (message.find("测试3") != std::string::npos) {
                ESP_LOGI(TAG, "使用激活音效播放直接消息");
                app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);
            } else if (message.find("测试4") != std::string::npos) {
                ESP_LOGI(TAG, "使用感叹音效播放直接消息");
                app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_EXCLAMATION);
            } else {
                ESP_LOGI(TAG, "使用感叹音效播放直接消息(默认)");
                app.Alert("TTS消息", message.c_str(), "happy", Lang::Sounds::P3_EXCLAMATION);
            }
        } else {
            ESP_LOGW(TAG, "设备忙，将消息加入队列。当前状态: %d", app.GetDeviceState());
            
            // 将消息添加到队列中
            std::lock_guard<std::mutex> lock(queue_mutex);
            tts_message_queue.push(message);
            
            ESP_LOGI(TAG, "消息已加入队列，当前队列长度: %d", tts_message_queue.size());
        }
    }
    
public:
    // 构造函数
    TtsSpeaker() : Thing("TtsSpeaker", "TTS语音播报器") {
        ESP_LOGI(TAG, "初始化TTS Speaker");
        
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
