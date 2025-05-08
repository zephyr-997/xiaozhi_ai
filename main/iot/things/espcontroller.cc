//ESPController文件 - ESP32控制器程序，用于通过MQTT控制家庭设备
#include "iot/thing.h"       // 物联网设备抽象层
#include "esp_log.h"         // ESP32日志系统
#include "esp_wifi.h"        // ESP32 WiFi功能
#include "esp_event.h"       // ESP32事件循环
#include "esp_netif.h"       // ESP32网络接口
#include <esp_mqtt.h>        // ESP32 MQTT客户端
// #include "D:\Study\ESP32CODE\xiaozhi-esp32\managed_components\78__esp-ml307\include\esp_mqtt.h"
#include "driver/uart.h"     // UART驱动，用于串口通信
#include <cstring>           // C字符串操作
#include <vector>            // 标准模板库向量容器
 
#define TAG "ESPController"  // 日志标签
 
// MQTT 配置 - 连接到MQTT服务器的设置
#define MQTT_URI       "mqtt://106.53.179.231:1883"  // MQTT服务器地址和端口
#define CLIENT_ID      "ESP32-Controller"            // 设备唯一标识符
#define MQTT_USERNAME  "admin"                       // MQTT服务器用户名
#define MQTT_PASSWORD  "azsxdcfv"                    // MQTT服务器密码
 
// MQTT 主题 - 用于控制不同设备的主题
#define LED_CMD_TOPIC  "HA-CMD-01/01/state"       // 客厅灯控制主题
#define FAN_CMD_TOPIC  "home/livingroom/fan/command"       // 风扇开关控制主题
#define FAN_PRESET_TOPIC "home/livingroom/fan/preset_mode" // 风扇速度控制主题
 
// UART 配置 - 用于串口通信的设置
#define UART_PORT UART_NUM_1 // 使用UART1端口
#define BUF_SIZE       (1024) // 接收缓冲区大小1KB
 
// 全局MQTT客户端实例
static esp_mqtt_client_handle_t mqtt_client;
 
namespace iot {
 
// ESPController类 - 继承自Thing基类的智能家居控制器
class ESPController : public Thing {
private:
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
        if (event_id == IP_EVENT_STA_GOT_IP) {
            // 获取到IP地址后启动MQTT客户端
            esp_mqtt_client_start(mqtt_client);
        }
    }
 
    // MQTT事件处理函数 - 处理MQTT连接和消息事件
    static void mqtt_event_handler(void* handler_args, esp_event_base_t base, 
                                  int32_t event_id, void* event_data) {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        if (event->event_id == MQTT_EVENT_CONNECTED) {
            // MQTT连接成功的处理
            ESP_LOGI(TAG, "MQTT Connected");
        }
        // 可以添加其他MQTT事件处理，如断开连接、接收消息等
    }
 
    // UART读取任务 - 持续读取串口命令并执行相应操作
    static void uart_read_task(void* arg) {
        uint8_t data[BUF_SIZE];
        while (1) {
            // 从UART读取数据，最多等待100ms
            int len = uart_read_bytes(UART_PORT, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));
            if (len > 0) {
                // 在数据末尾添加字符串结束符
                data[len] = '\0';
                std::string command((char*)data);
                // 移除行尾的回车换行符
                command.erase(command.find_last_not_of("\r\n") + 1);
 
                // 根据接收到的指令执行相应的操作
                if (command.find("打开客厅灯") != std::string::npos) {
                    // 识别到开灯指令，发送MQTT消息
                    send_mqtt_command(LED_CMD_TOPIC, "ON");
                } else if (command.find("关闭客厅灯") != std::string::npos) {
                    // 识别到关灯指令，发送MQTT消息
                    send_mqtt_command(LED_CMD_TOPIC, "OFF");
                } else if (command.find("打开风扇") != std::string::npos) {
                    // 识别到开风扇指令，发送MQTT消息
                    send_mqtt_command(FAN_CMD_TOPIC, "ON");
                } else if (command.find("关闭风扇") != std::string::npos) {
                    // 识别到关风扇指令，发送MQTT消息
                    send_mqtt_command(FAN_CMD_TOPIC, "OFF");
                } else if (command.find("一挡") != std::string::npos) {
                    // 识别到风扇一档指令，设置为低速
                    send_mqtt_command(FAN_PRESET_TOPIC, "Low");
                } else if (command.find("二挡") != std::string::npos) {
                    // 识别到风扇二档指令，设置为中速
                    send_mqtt_command(FAN_PRESET_TOPIC, "Medium");
                } else if (command.find("三挡") != std::string::npos) {
                    // 识别到风扇三档指令，设置为高速
                    send_mqtt_command(FAN_PRESET_TOPIC, "High");
                }
            }else {
                // 未收到数据时短暂延时，减轻CPU负担
                vTaskDelay(pdMS_TO_TICKS(10));}
        }
    }
 
    // MQTT命令发送函数 - 将命令通过MQTT发送到指定主题
    static void send_mqtt_command(const char* topic, const char* payload) {
        // 发布MQTT消息，QoS为1表示至少一次送达
        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
        // 记录日志：已发送的指令及内容
        ESP_LOGI(TAG, "发送指令: %s -> %s", topic, payload);
    }
 
    public:
    // 构造函数 - 初始化控制器及其功能
    ESPController() : Thing("LivingRoomController", "客厅设备控制器") {
    // 确保网络接口只初始化一次
    static bool netif_initialized = false;
    if (!netif_initialized) {
        // 初始化网络接口和事件循环
        esp_netif_init();
        esp_event_loop_create_default();
        netif_initialized = true;
    }
 
        // 初始化MQTT客户端配置
        esp_mqtt_client_config_t mqtt_cfg = {};
        mqtt_cfg.broker.address.uri = MQTT_URI;                 // 设置MQTT服务器地址
        mqtt_cfg.credentials.client_id = CLIENT_ID;             // 设置客户端ID
        mqtt_cfg.credentials.username = MQTT_USERNAME;          // 设置用户名
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;  // 设置密码
        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);          // 初始化MQTT客户端
 
        // 注册MQTT事件处理回调
        esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
        // 启动MQTT客户端（此处假设网络已连接）
        esp_mqtt_client_start(mqtt_client);
 
        // 配置UART参数
        uart_config_t uart_config = {
            .baud_rate = 115200,                // 波特率115200
            .data_bits = UART_DATA_8_BITS,      // 8位数据位
            .parity = UART_PARITY_DISABLE,      // 无校验
            .stop_bits = UART_STOP_BITS_1,      // 1位停止位
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, // 无流控
        };
        uart_param_config(UART_PORT, &uart_config);                // 配置UART参数
        uart_driver_install(UART_PORT, BUF_SIZE * 2, 0, 0, NULL, 0); // 安装UART驱动
 
        // 创建UART读取任务
        xTaskCreate(uart_read_task, "uart_task", 4096, NULL, 2, NULL);
 
        // 注册设备属性 - 客厅灯状态属性
        properties_.AddBooleanProperty("light_status", "客厅灯状态", [this]() -> bool {
            return false; // 此处简单返回false，实际应当反映真实状态
        });
 
        // 注册控制方法 - 控制客厅灯开关
        methods_.AddMethod("ControlLight", "控制客厅灯开关",
            ParameterList(std::vector<Parameter>{Parameter("state", "开关状态", kValueTypeString)}),
            [this](const ParameterList& params) {
                // 发送MQTT命令控制灯光
                send_mqtt_command(LED_CMD_TOPIC, params["state"].string().c_str());
            }
        );
 
        // 注册控制方法 - 控制风扇开关
        methods_.AddMethod("ControlFan", "控制风扇开关",
            ParameterList(std::vector<Parameter>{Parameter("state", "开关状态", kValueTypeString)}),
            [this](const ParameterList& params) {
                // 发送MQTT命令控制风扇
                std::string state = params["state"].string();
                send_mqtt_command(FAN_CMD_TOPIC, state.c_str());
            }
        );
 
        // 注册控制方法 - 设置风扇速度
        methods_.AddMethod("SetFanSpeed", "设置风扇档位",
            ParameterList(std::vector<Parameter>{Parameter("speed", "档位（Low/Medium/High）", kValueTypeString)}),
            [this](const ParameterList& params) {
                // 发送MQTT命令设置风扇速度
                std::string speed = params["speed"].string();
                send_mqtt_command(FAN_PRESET_TOPIC, speed.c_str());
            }
        );
    }
};
 
} // namespace iot
 
// 声明并注册ESPController为Thing实例
DECLARE_THING(ESPController);
