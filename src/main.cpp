// 引入 Arduino 核心库
#include <Arduino.h>
// 引入 Wire 库，用于 I2C
#include <Wire.h>
// 引入 WiFi 库，用于网络接收视频流
#include <WiFi.h>
// 引入 ESP32-S3 官方底层 GPIO 寄存器宏定义
#include "soc/gpio_reg.h"

// ==========================================
// 0. 网络及显示配置
// ==========================================
const char* ssid = "PCDN1";          // 替换为你的WiFi名称
const char* password = "431227200";  // 替换为你的WiFi密码
const int tcp_port = 8080;                    // TCP 服务端端口
WiFiServer server(tcp_port);

// 屏幕分辨率配置 (250次时钟 * 8像素 = 2000像素宽，1500行)
#define EPD_WIDTH 2000
#define EPD_HEIGHT 1500
#define EPD_CLK_PER_LINE (EPD_WIDTH / 8) // 250
#define FRAME_SIZE (EPD_CLK_PER_LINE * EPD_HEIGHT) // 375,000 Bytes

// M1 模拟波形重刷次数 (根据显示质量调整，1=极速但可能泛白，2~3=对比度好)
#define M1_PHASES 2 

// ==========================================
// 1. 终极真理引脚定义
// ==========================================
#define PIN_SDA 39
#define PIN_SCL 40
#define PCA9555_ADDR 0x20
#define TPS65185_ADDR 0x68

#define PIN_CKH  4
#define PIN_CKV  48
#define PIN_STH  41
#define PIN_LEH  42
#define PIN_STV  45

// 16 位数据总线
const int DATA_PINS[16] = {5, 6, 7, 15, 16, 17, 18, 8, 9, 10, 11, 12, 13, 14, 21, 47};

uint8_t pca_port0 = 0x00;
uint8_t pca_port1 = 0x00;

// 全局图像帧缓冲区 (分配在 PSRAM 中)
uint8_t* frame_buffer = nullptr;

// ==========================================
// 2. 超高速 GPIO LUT (查找表)
// ==========================================
// 预先计算 0~255 每个字节对应的寄存器操作掩码，彻底消灭循环内的分支判断
uint32_t map_low_set[256];
uint32_t map_low_clear[256];
uint32_t map_high_set[256];
uint32_t map_high_clear[256];

void build_fast_lut() {
    Serial.println("⚙️ Building high-speed GPIO LUT...");
    for (int byte_val = 0; byte_val < 256; byte_val++) {
        uint16_t bus_val = 0;
        // 将 1-bit 的 8 个像素展开为 16-bit 总线数据
        for (int i = 0; i < 8; i++) {
            // 假设 byte 的高位对应屏幕的左侧像素
            bool is_white = (byte_val & (1 << (7 - i))) != 0;
            // 编码：White = 10(二进制), Black = 01(二进制) - 对应原代码0xAAAA和0x5555逻辑
            if (is_white) {
                bus_val |= (1 << (2 * i + 1)); // 设为 10
            } else {
                bus_val |= (1 << (2 * i));     // 设为 01
            }
        }

        uint32_t l_set = 0, l_clear = 0, h_set = 0, h_clear = 0;
        for (int b = 0; b < 16; b++) {
            int pin = DATA_PINS[b];
            bool bit_high = (bus_val & (1 << b)) != 0;
            if (pin < 32) {
                if (bit_high) l_set |= (1UL << pin);
                else l_clear |= (1UL << pin);
            } else {
                if (bit_high) h_set |= (1UL << (pin - 32));
                else h_clear |= (1UL << (pin - 32));
            }
        }
        map_low_set[byte_val] = l_set;
        map_low_clear[byte_val] = l_clear;
        map_high_set[byte_val] = h_set;
        map_high_clear[byte_val] = h_clear;
    }
}

// ==========================================
// 3. I2C 及周边硬件配置
// ==========================================
bool check_i2c_device(uint8_t addr) {
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}

bool i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(data);
    return (Wire.endTransmission() == 0);
}

uint8_t i2c_read_reg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint16_t)addr, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return 0x00;
}

void init_pca9555() {
    i2c_write_reg(PCA9555_ADDR, 0x06, 0xFF);
    i2c_write_reg(PCA9555_ADDR, 0x07, 0xC4);
    pca_port0 = 0x00; pca_port1 = 0x00;
    i2c_write_reg(PCA9555_ADDR, 0x02, pca_port0);
    i2c_write_reg(PCA9555_ADDR, 0x03, pca_port1);
}

void set_pca_pin(int port, int pin, bool high) {
    if (port == 0) {
        if (high) pca_port0 |= (1 << pin); else pca_port0 &= ~(1 << pin);
        i2c_write_reg(PCA9555_ADDR, 0x02, pca_port0);
    } else {
        if (high) pca_port1 |= (1 << pin); else pca_port1 &= ~(1 << pin);
        i2c_write_reg(PCA9555_ADDR, 0x03, pca_port1);
    }
}

bool power_on_tps65185() {
    Serial.println("🔌 [PMIC] Waking up TPS65185...");
    set_pca_pin(1, 5, false); delay(100);
    set_pca_pin(1, 5, true); delay(150);

    if (!check_i2c_device(TPS65185_ADDR)) {
        Serial.println("❌ ERROR: TPS65185 DID NOT WAKE UP!");
        return false;
    }
    i2c_write_reg(TPS65185_ADDR, 0x03, 0xE1);
    i2c_write_reg(TPS65185_ADDR, 0x04, 0xAA);
    set_pca_pin(1, 3, true); set_pca_pin(1, 4, true);
    delay(50);
    i2c_write_reg(TPS65185_ADDR, 0x08, 0x3F);

    int timeout = 50; bool pg_ok = false;
    while(timeout--) {
        uint8_t pg = i2c_read_reg(TPS65185_ADDR, 0x0A);
        if (pg != 0xFF && (pg & 0xF8) == 0xF8) { pg_ok = true; break; }
        delay(10);
    }
    if(!pg_ok) Serial.println("⚠️ WARNING: PG check bypassed.");
    else Serial.println(" OK! High voltage is LIVE!");

    i2c_write_reg(TPS65185_ADDR, 0x0B, 0x9C);
    set_pca_pin(1, 0, true); set_pca_pin(1, 1, true);
    return true;
}

void init_bare_metal_pins() {
    pinMode(PIN_CKH, OUTPUT); pinMode(PIN_CKV, OUTPUT);
    pinMode(PIN_STH, OUTPUT); pinMode(PIN_LEH, OUTPUT); pinMode(PIN_STV, OUTPUT);
    for (int i = 0; i < 16; i++) pinMode(DATA_PINS[i], INPUT_PULLUP);
    digitalWrite(PIN_CKH, LOW); digitalWrite(PIN_CKV, LOW);
    digitalWrite(PIN_STH, LOW); digitalWrite(PIN_LEH, LOW); digitalWrite(PIN_STV, LOW);
}

void set_bus_direction(bool is_output) {
    for (int i = 0; i < 16; i++) pinMode(DATA_PINS[i], is_output ? OUTPUT : INPUT_PULLUP);
}

// ==========================================
// 4. M1 极限动态刷帧引擎 (支持真实图像缓冲)
// ==========================================
void drive_image_fast(uint8_t* fb) {
    set_bus_direction(true);

    // 帧起始信号
    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_STV - 32))); delayMicroseconds(2);
    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(2);
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32)));
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_STV - 32))); delayMicroseconds(2);

    int ptr = 0;
    for (int line = 0; line < EPD_HEIGHT; line++) {
        // 行起始信号
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_STH - 32))); delayMicroseconds(2);
        REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << PIN_CKH)); delayMicroseconds(2);
        REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << PIN_CKH));
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_STH - 32))); delayMicroseconds(2);

        // 🚀 像素时钟超频爆发：无分支查找表直写！
        for (int p = 0; p < EPD_CLK_PER_LINE; p++) {
            uint8_t pix_byte = fb[ptr++];
            
            // 核心级优化：硬件自动忽略向 W1TS/W1TC 写 0，无需用 if 阻断，实现绝对流水线执行
            REG_WRITE(GPIO_OUT_W1TS_REG, map_low_set[pix_byte]);
            REG_WRITE(GPIO_OUT_W1TC_REG, map_low_clear[pix_byte]);
            REG_WRITE(GPIO_OUT1_W1TS_REG, map_high_set[pix_byte]);
            REG_WRITE(GPIO_OUT1_W1TC_REG, map_high_clear[pix_byte]);

            // 翻转时钟
            REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << PIN_CKH));
            asm volatile("nop; nop;");
            REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << PIN_CKH));
            asm volatile("nop; nop;");
        }

        // 锁存及行切换信号
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_LEH - 32))); delayMicroseconds(2);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_LEH - 32))); delayMicroseconds(2);
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(2);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(2);
    }
    set_bus_direction(false);
}

// ==========================================
// 5. 主循环与网络流处理
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n====== BAD APPLE M1 VIDEO STREAM ======");

    // 分配 PSRAM 显存
    frame_buffer = (uint8_t*)ps_malloc(FRAME_SIZE);
    if (!frame_buffer) {
        Serial.println("❌ PSRAM allocation failed! Please enable PSRAM in Arduino IDE.");
        while(1) delay(1000);
    }
    memset(frame_buffer, 0xFF, FRAME_SIZE); // 默认全白

    build_fast_lut();

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
    init_pca9555();
    power_on_tps65185();
    init_bare_metal_pins();

    WiFi.begin(ssid, password);
    Serial.print("📡 Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n✅ WiFi Connected. IP Address: ");
    Serial.println(WiFi.localIP());

    server.begin();
    Serial.printf("🎬 TCP Video Server started on port %d\n", tcp_port);
}

void loop() {
    WiFiClient client = server.available();
    if (client) {
        Serial.println("💻 Python Client Connected!");
        
        while (client.connected()) {
            // 接收一帧数据
            size_t bytes_read = 0;
            // 增加简单的帧头校验防止错位 (Python端发送 8 字节 "FRAMEINC")
            uint8_t header[8];
            if (client.readBytes(header, 8) != 8) continue;
            if (memcmp(header, "FRAMEINC", 8) != 0) continue; 

            // 循环拉取 375KB 图像本体
            while (bytes_read < FRAME_SIZE && client.connected()) {
                int available = client.available();
                if (available) {
                    int read_len = client.read(frame_buffer + bytes_read, FRAME_SIZE - bytes_read);
                    if (read_len > 0) bytes_read += read_len;
                } else {
                    delay(1);
                }
            }

            if (bytes_read == FRAME_SIZE) {
                // 模拟 M1 波形：重复刷入同一帧以提高对比度
                for(int i = 0; i < M1_PHASES; i++){
                    drive_image_fast(frame_buffer);
                }
                // 向客户端回复 ACK 允许发送下一帧，做背压控制
                client.write("ACK"); 
            }
        }
        Serial.println("Disconnected.");
    }
}