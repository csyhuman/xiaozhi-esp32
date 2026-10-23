/**
 * generic_tools.h  —  小智 ESP32-S3 固件「通用外设工具」
 * ---------------------------------------------------------------
 * 只注册一次，以后加传感器不用再改固件、不用再重编译。
 *
 * 提供的 MCP 工具：
 *   self.gpio.write   pin, level(0/1)            点灯 / 继电器 / 蜂鸣器
 *   self.gpio.read    pin                        按键 / 人体红外 / 门磁 / 水位
 *   self.adc.read     pin(1-10)                  光敏 / 土壤湿度 / 电位器
 *   self.pwm.set      pin, freq, duty(0-100)     电机调速 / 呼吸灯
 *   self.servo.set    pin, angle(0-180)          舵机
 *   self.i2c.read     addr, reg, len             绝大多数 I2C 传感器
 *   self.i2c.write    addr, reg, data(hex)       I2C 配置 / 下发命令
 *
 * 使用前提（必读）：
 *   1. main/CMakeLists.txt 的 PRIV_REQUIRES 里加一行  esp_adc
 *      （仓库默认只带了 esp_driver_i2c / esp_driver_ledc / esp_driver_gpio，
 *        不含 ADC 组件，不加会报 esp_adc/adc_oneshot.h: No such file）
 *   2. 目标芯片 ESP32-S3，ESP-IDF release/v6.1（仓库要求 >= 6.0.1）
 *
 * 已验证：API 签名与 ESP-IDF release/v6.1 头文件一致
 *         （i2c_new_master_bus / i2c_master_bus_add_device / i2c_master_transmit /
 *           i2c_master_transmit_receive / i2c_master_receive /
 *           adc_oneshot_new_unit / adc_oneshot_config_channel / adc_oneshot_read）
 * 未验证：本地无 ESP-IDF 环境，未做过完整编译；如有报错请对照错误行微调。
 */

#ifndef _GENERIC_TOOLS_H_
#define _GENERIC_TOOLS_H_

#include "mcp_server.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>

#include <cstdio>
#include <string>

// =====================================================================
// 唯一需要按自己接线改的地方：通用 I2C 总线用哪两个脚
// 必须避开：0/3/45/46(启动脚) 19/20(USB) 26-32(Flash/PSRAM) 43/44(串口0)
// 以及板子上已经用掉的音频/屏幕/按键脚
// =====================================================================
#ifndef GEN_I2C_SDA
#define GEN_I2C_SDA   GPIO_NUM_8
#endif
#ifndef GEN_I2C_SCL
#define GEN_I2C_SCL   GPIO_NUM_9
#endif

#define GEN_TAG "GenericTools"

class GenericTools {
public:
    void Register() {
        auto& mcp = McpServer::GetInstance();

        // ============ 1. GPIO 输出 ============
        mcp.AddTool(
            "self.gpio.write",
            "设置任意GPIO输出高低电平。用于点亮LED、吸合继电器、拉响蜂鸣器。"
            "pin为GPIO编号(0-48)，level为1(高电平/开)或0(低电平/关)。",
            PropertyList({
                Property("pin",   kPropertyTypeInteger, 0, 0, 48),
                Property("level", kPropertyTypeInteger, 1, 0, 1)
            }),
            [](const PropertyList& p) -> ReturnValue {
                int pin = p["pin"].value<int>();
                int lv  = p["level"].value<int>();
                if (pin < 0 || pin > 48) return std::string("pin 超出范围 0-48");
                gpio_config_t cfg = {
                    .pin_bit_mask = (1ULL << pin),
                    .mode         = GPIO_MODE_OUTPUT,
                    .pull_up_en   = GPIO_PULLUP_DISABLE,
                    .pull_down_en = GPIO_PULLDOWN_DISABLE,
                    .intr_type    = GPIO_INTR_DISABLE,
                };
                if (gpio_config(&cfg) != ESP_OK) return std::string("GPIO配置失败");
                gpio_set_level((gpio_num_t)pin, lv);
                ESP_LOGI(GEN_TAG, "gpio.write pin=%d level=%d", pin, lv);
                return true;
            });

        // ============ 2. GPIO 输入 ============
        mcp.AddTool(
            "self.gpio.read",
            "读取某个GPIO的电平，返回0或1。用于按键、人体红外模块、水位传感器、"
            "门磁开关等数字量传感器。内部已开启上拉。",
            PropertyList({
                Property("pin", kPropertyTypeInteger, 0, 0, 48)
            }),
            [](const PropertyList& p) -> ReturnValue {
                int pin = p["pin"].value<int>();
                if (pin < 0 || pin > 48) return std::string("pin 超出范围 0-48");
                gpio_config_t cfg = {
                    .pin_bit_mask = (1ULL << pin),
                    .mode         = GPIO_MODE_INPUT,
                    .pull_up_en   = GPIO_PULLUP_ENABLE,
                    .pull_down_en = GPIO_PULLDOWN_DISABLE,
                    .intr_type    = GPIO_INTR_DISABLE,
                };
                if (gpio_config(&cfg) != ESP_OK) return std::string("GPIO配置失败");
                return (int)gpio_get_level((gpio_num_t)pin);
            });

        // ============ 3. ADC 模拟量 ============
        mcp.AddTool(
            "self.adc.read",
            "读取模拟电压，返回0-4095的原始值。ESP32-S3只有GPIO1到GPIO10支持ADC。"
            "用于光敏电阻、土壤湿度、电位器、声音传感器等模拟量传感器。"
            "数值越大代表电压越高（3.3V对应4095左右）。",
            PropertyList({
                Property("pin", kPropertyTypeInteger, 1, 1, 10)
            }),
            [](const PropertyList& p) -> ReturnValue {
                int pin = p["pin"].value<int>();
                if (pin < 1 || pin > 10) return std::string("S3 的ADC只支持 GPIO1-GPIO10");

                static adc_oneshot_unit_handle_t unit = nullptr;
                if (unit == nullptr) {
                    adc_oneshot_unit_init_cfg_t ucfg = {};
                    ucfg.unit_id = ADC_UNIT_1;
                    ucfg.clk_src = ADC_RTC_CLK_SRC_DEFAULT;
                    ucfg.ulp_mode = ADC_ULP_MODE_DISABLE;
                    if (adc_oneshot_new_unit(&ucfg, &unit) != ESP_OK) {
                        return std::string("ADC 初始化失败");
                    }
                }
                adc_channel_t ch = (adc_channel_t)(pin - 1);  // GPIO1->CH0 ... GPIO10->CH9
                adc_oneshot_chan_cfg_t ccfg = {};
                ccfg.atten    = ADC_ATTEN_DB_12;    // 量程约 0-3.1V
                ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
                if (adc_oneshot_config_channel(unit, ch, &ccfg) != ESP_OK) {
                    return std::string("ADC 通道配置失败");
                }
                int raw = 0;
                if (adc_oneshot_read(unit, ch, &raw) != ESP_OK) {
                    return std::string("ADC 读取失败");
                }
                ESP_LOGI(GEN_TAG, "adc.read pin=%d raw=%d", pin, raw);
                return raw;
            });

        // ============ 4. PWM（电机 / 呼吸灯）============
        mcp.AddTool(
            "self.pwm.set",
            "在指定GPIO输出PWM。freq为频率(Hz, 1-5000)，duty为占空比(0-100的整数)。"
            "直流电机调速用 freq=1000，duty=速度；呼吸灯用 freq=1000，duty=亮度。"
            "如果要转舵机，请改用 self.servo.set，那个精度更高。最多同时2路。",
            PropertyList({
                Property("pin",  kPropertyTypeInteger, 0, 0, 48),
                Property("freq", kPropertyTypeInteger, 1000, 1, 5000),
                Property("duty", kPropertyTypeInteger, 0, 0, 100)
            }),
            [](const PropertyList& p) -> ReturnValue {
                int slot = AcquireSlot(p["pin"].value<int>(), p["freq"].value<int>());
                if (slot < 0) return std::string("PWM资源已满(最多2路)或初始化失败");
                SetDuty13(slot, (uint32_t)p["duty"].value<int>() * 8191 / 100);
                return true;
            });

        // ============ 5. 舵机 ============
        mcp.AddTool(
            "self.servo.set",
            "把舵机转到指定角度并保持。角度0-180(90为中间位置)。"
            "适用于SG90/MG90S等普通舵机，信号线接指定GPIO，舵机需独立5V供电并与开发板共地。",
            PropertyList({
                Property("pin",   kPropertyTypeInteger, 0, 0, 48),
                Property("angle", kPropertyTypeInteger, 90, 0, 180)
            }),
            [](const PropertyList& p) -> ReturnValue {
                int pin   = p["pin"].value<int>();
                int angle = p["angle"].value<int>();
                if (angle < 0)   angle = 0;
                if (angle > 180) angle = 180;
                int slot = AcquireSlot(pin, 50);   // 舵机固定 50Hz
                if (slot < 0) return std::string("PWM资源已满(最多2路)或初始化失败");
                // 0.5ms~2.5ms 脉宽 -> 13bit(8192) 占空比, 周期 20ms
                uint32_t us   = 500 + (uint32_t)angle * 2000 / 180;
                uint32_t duty = us * 8192 / 20000;
                SetDuty13(slot, duty);
                ESP_LOGI(GEN_TAG, "servo.set pin=%d angle=%d duty=%u", pin, angle,
                         (unsigned)duty);
                return true;
            });

        // ============ 6. I2C 读 ============
        mcp.AddTool(
            "self.i2c.read",
            "从I2C传感器读取寄存器数据。addr为7位地址的十进制值(例如0x23要写35)，"
            "reg为寄存器/命令字节(十进制，不需要时写-1)，len为读取字节数(1-16)。"
            "返回大写十六进制字符串，例如\"0A1F\"。用于光照、温湿度、气压、六轴等I2C传感器。",
            PropertyList({
                Property("addr", kPropertyTypeInteger, 0, 0, 127),
                Property("reg",  kPropertyTypeInteger, 0, -1, 255),
                Property("len",  kPropertyTypeInteger, 2, 1, 16)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                return I2cTransact(p["addr"].value<int>(),
                                   p["reg"].value<int>(),
                                   std::string(), 
                                   p["len"].value<int>());
            });

        // ============ 7. I2C 写 ============
        mcp.AddTool(
            "self.i2c.write",
            "向I2C设备写入数据。addr为7位地址的十进制值，reg为寄存器/命令字节(十进制)，"
            "data为十六进制字节串(如\"01\"或\"0A0B\"，长度必须为偶数)。"
            "用于初始化传感器或向I2C继电器/扩展板下发控制命令。",
            PropertyList({
                Property("addr", kPropertyTypeInteger, 0, 0, 127),
                Property("reg",  kPropertyTypeInteger, 0, 0, 255),
                Property("data", kPropertyTypeString, std::string("")).SetMaxLength(64)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                return I2cTransact(p["addr"].value<int>(),
                                   p["reg"].value<int>(),
                                   p["data"].value<std::string>(),
                                   0);
            });

        ESP_LOGI(GEN_TAG, "已注册 7 个通用外设工具");
    }

private:
    // ---------------- PWM 通道管理（2 路：TIMER/CHANNEL 2 和 3）----------------
    struct Slot {
        gpio_num_t pin;
        bool used;
    };

    static Slot* Slots() {
        static Slot s[2] = { { GPIO_NUM_NC, false }, { GPIO_NUM_NC, false } };
        return s;
    }

    static ledc_timer_t TimerOf(int idx) {
        return (idx == 0) ? LEDC_TIMER_2 : LEDC_TIMER_3;
    }
    static ledc_channel_t ChannelOf(int idx) {
        return (idx == 0) ? LEDC_CHANNEL_2 : LEDC_CHANNEL_3;
    }

    // 返回槽位下标；-1 表示资源已满或配置失败
    static int AcquireSlot(int pin, int freq_hz) {
        Slot* s = Slots();
        int free_idx = -1;
        for (int i = 0; i < 2; i++) {
            if (s[i].used && s[i].pin == (gpio_num_t)pin) return i;   // 该脚已在用
            if (!s[i].used && free_idx < 0) free_idx = i;
        }
        if (free_idx < 0) return -1;
        if (freq_hz < 1)    freq_hz = 1;
        if (freq_hz > 5000) freq_hz = 5000;

        ledc_timer_config_t t = {};
        t.speed_mode      = LEDC_LOW_SPEED_MODE;
        t.timer_num       = TimerOf(free_idx);
        t.duty_resolution = LEDC_TIMER_13_BIT;
        t.freq_hz         = (uint32_t)freq_hz;
        t.clk_cfg         = LEDC_AUTO_CLK;
        if (ledc_timer_config(&t) != ESP_OK) return -1;

        ledc_channel_config_t c = {};
        c.gpio_num   = (gpio_num_t)pin;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel    = ChannelOf(free_idx);
        c.timer_sel  = TimerOf(free_idx);
        c.duty       = 0;
        c.hpoint     = 0;
        if (ledc_channel_config(&c) != ESP_OK) return -1;

        s[free_idx].pin  = (gpio_num_t)pin;
        s[free_idx].used = true;
        return free_idx;
    }

    static void SetDuty13(int idx, uint32_t duty) {
        if (duty > 8191) duty = 8191;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ChannelOf(idx), duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ChannelOf(idx));
    }

    // ---------------- I2C ----------------
    i2c_master_bus_handle_t bus_ = nullptr;

    void EnsureBus() {
        if (bus_ != nullptr) return;
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port  = I2C_NUM_1;          // 用 1 号总线，别抢屏幕的 0 号
        cfg.sda_io_num = GEN_I2C_SDA;
        cfg.scl_io_num = GEN_I2C_SCL;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt = 7;
        cfg.flags.enable_internal_pullup = 1;
        if (i2c_new_master_bus(&cfg, &bus_) != ESP_OK) {
            ESP_LOGE(GEN_TAG, "I2C 总线初始化失败 (SDA=%d SCL=%d)",
                     (int)GEN_I2C_SDA, (int)GEN_I2C_SCL);
            bus_ = nullptr;
        }
    }

    static int HexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // rd_len > 0 表示读，返回十六进制串；rd_len == 0 表示写
    std::string I2cTransact(int addr, int reg, const std::string& hex, int rd_len) {
        EnsureBus();
        if (bus_ == nullptr) return std::string("I2C总线初始化失败");

        i2c_device_config_t dcfg = {};
        dcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dcfg.device_address  = (uint16_t)addr;
        dcfg.scl_speed_hz    = 100000;
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(bus_, &dcfg, &dev) != ESP_OK) {
            return std::string("I2C设备挂载失败(地址可能不对)");
        }

        std::string ret = "ok";
        uint8_t reg_byte = (uint8_t)reg;

        if (rd_len > 0) {
            if (rd_len > 16) rd_len = 16;
            uint8_t buf[16] = {0};
            esp_err_t err;
            if (reg >= 0) {
                err = i2c_master_transmit_receive(dev, &reg_byte, 1, buf, (size_t)rd_len, 100);
            } else {
                err = i2c_master_receive(dev, buf, (size_t)rd_len, 100);
            }
            if (err == ESP_OK) {
                char tmp[4];
                ret.clear();
                for (int i = 0; i < rd_len; i++) {
                    snprintf(tmp, sizeof(tmp), "%02X", buf[i]);
                    ret += tmp;
                }
                ESP_LOGI(GEN_TAG, "i2c.read addr=0x%02X reg=%d -> %s", addr, reg, ret.c_str());
            } else {
                ret = "读失败 err=" + std::to_string((int)err) +
                      " (检查接线/上拉电阻/地址)";
            }
        } else {
            uint8_t buf[33] = {0};
            int n = 0;
            if (reg >= 0) buf[n++] = reg_byte;
            if (hex.size() % 2 != 0) {
                i2c_master_bus_rm_device(dev);
                return std::string("data 必须是偶数长度的十六进制串，例如 \"01\"");
            }
            for (size_t i = 0; i + 1 < hex.size() && n < 32; i += 2) {
                int h = HexVal(hex[i]);
                int l = HexVal(hex[i + 1]);
                if (h < 0 || l < 0) {
                    i2c_master_bus_rm_device(dev);
                    return std::string("data 含非法字符，只能是 0-9A-F");
                }
                buf[n++] = (uint8_t)((h << 4) | l);
            }
            esp_err_t err = i2c_master_transmit(dev, buf, (size_t)n, 100);
            if (err != ESP_OK) {
                ret = "写失败 err=" + std::to_string((int)err) +
                      " (检查接线/上拉电阻/地址)";
            } else {
                ESP_LOGI(GEN_TAG, "i2c.write addr=0x%02X reg=%d %d字节", addr, reg, n);
            }
        }

        i2c_master_bus_rm_device(dev);
        return ret;
    }
};

#endif  // _GENERIC_TOOLS_H_
