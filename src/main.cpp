#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "esp_lcd_panel_io.h"

// ==========================================
// 0. 网络配置
// ==========================================
const char* ssid = "PCDN1";
const char* password = "431227200";
const int tcp_port = 8080;
WiFiServer server(tcp_port);

// ==========================================
// 1. 物理屏幕与居中视频参数
// ==========================================
#define EPD_WIDTH 1872
#define EPD_HEIGHT 1404
#define EPD_BYTES_PER_LINE (EPD_WIDTH / 8) // 每行 234 个时钟周期

#define VIDEO_WIDTH 1440
#define VIDEO_HEIGHT 1080
// 1440 * 1080 / 8 = 194400 字节，精确接收
#define VIDEO_PAYLOAD_SIZE 194400 

#define PAD_TOP 162
#define PAD_LEFT_CLKS 27  // (1872 - 1440) / 2 / 8像素
#define VIDEO_CLKS 180    // 1440 / 8像素

#define M1_PHASES 1 

#define PIN_SDA 39
#define PIN_SCL 40
#define PCA9555_ADDR 0x20
#define TPS65185_ADDR 0x68

#define PIN_CKH  4
#define PIN_CKV  48
#define PIN_STH  41
#define PIN_LEH  42
#define PIN_STV  45

const int DATA_PINS[16] = {5, 6, 7, 15, 16, 17, 18, 8, 9, 10, 11, 12, 13, 14, 21, 47};

// ==========================================
// 2. 图像缓冲与高速查找表
// ==========================================
// 显存直接分配在高速 PSRAM 中：1404 行 * 234 字 = 328,536 个 uint16_t (~657KB)
uint16_t* frame_buffer = nullptr;
uint8_t* py_buffer = nullptr; 
uint16_t lut_expand[256];

esp_lcd_panel_io_handle_t io_handle = NULL;
volatile bool dma_done = true;

// DMA 传输完成中断
bool color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    dma_done = true;
    return false;
}

// 构建 1Byte -> 16-bit (8个像素) 的硬件扩张映射表
void init_lut() {
    for(int i=0; i<256; i++) {
        uint16_t val = 0;
        for(int b=0; b<8; b++) {
            bool white = (i & (1 << (7 - b)));
            if(white) val |= (2 << (b * 2)); // 白色输出 10
            else      val |= (1 << (b * 2)); // 黑色输出 01
        }
        lut_expand[i] = val;
    }
}

// 快速铺满纯色背景
void set_background(uint16_t color) {
    for(int i = 0; i < EPD_HEIGHT * EPD_BYTES_PER_LINE; i++) {
        frame_buffer[i] = color;
    }
}

// 查表法极速将 Python 视频帧嵌入居中位置
void update_video_region(uint8_t* py_data) {
    int py_ptr = 0;
    for (int y = 0; y < EPD_HEIGHT; y++) {
        if (y >= PAD_TOP && y < (PAD_TOP + VIDEO_HEIGHT)) {
            int offset = y * EPD_BYTES_PER_LINE + PAD_LEFT_CLKS;
            for (int i = 0; i < VIDEO_CLKS; i++) {
                frame_buffer[offset + i] = lut_expand[py_data[py_ptr++]];
            }
        }
    }
}

// ==========================================
// 3. I2C 及 电源管理
// ==========================================
bool i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data) {
    Wire.beginTransmission(addr); Wire.write(reg); Wire.write(data);
    return (Wire.endTransmission() == 0);
}
void init_pca9555() {
    i2c_write_reg(PCA9555_ADDR, 0x06, 0xFF); i2c_write_reg(PCA9555_ADDR, 0x07, 0xC4);
    i2c_write_reg(PCA9555_ADDR, 0x02, 0x00); i2c_write_reg(PCA9555_ADDR, 0x03, 0x00);
}
void set_pca_pin(int port, int pin, bool high) {
    static uint8_t p0 = 0, p1 = 0;
    if (port == 0) {
        if (high) p0 |= (1 << pin); else p0 &= ~(1 << pin);
        i2c_write_reg(PCA9555_ADDR, 0x02, p0);
    } else {
        if (high) p1 |= (1 << pin); else p1 &= ~(1 << pin);
        i2c_write_reg(PCA9555_ADDR, 0x03, p1);
    }
}
void power_on_tps65185() {
    Serial.println("🔌 启动高压...");
    set_pca_pin(1, 5, false); delay(100); set_pca_pin(1, 5, true); delay(200);
    i2c_write_reg(TPS65185_ADDR, 0x03, 0xE1); i2c_write_reg(TPS65185_ADDR, 0x04, 0xAA);
    set_pca_pin(1, 3, true); set_pca_pin(1, 4, true); delay(100);
    i2c_write_reg(TPS65185_ADDR, 0x08, 0x3F); i2c_write_reg(TPS65185_ADDR, 0x0B, 0x9C);
    set_pca_pin(1, 0, true); set_pca_pin(1, 1, true); delay(100);
}

// ==========================================
// 4. 极致混合驱动 (CPU 控制时序 + DMA 狂轰数据)
// ==========================================
void push_frame() {
    // 帧起始：直接寄存器操作 STV 与 CKV
    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_STV - 32))); delayMicroseconds(1);
    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(1);
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32)));
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_STV - 32))); delayMicroseconds(1);

    for (int y = 0; y < EPD_HEIGHT; y++) {
        dma_done = false;
        int offset = y * EPD_BYTES_PER_LINE;
        
        // 极致借位法：
        // 第 1 个数据通过 CMD 发送，此时 DC (STH) 会自动被拉高 1 个时钟周期！
        uint16_t cmd = frame_buffer[offset]; 
        // 余下的 233 个数据通过 DATA 发送，此时 DC (STH) 自动保持低电平！
        void* param = &frame_buffer[offset + 1];
        size_t param_size = (EPD_BYTES_PER_LINE - 1) * 2; // 必须是字节数
        
        esp_lcd_panel_io_tx_color(io_handle, cmd, param, param_size);
        
        // 极速空转，阻塞等待这一行的 DMA 打完 (~20us)
        while(!dma_done) { __asm__("nop"); }

        // 行锁存与切换：直接通过高速寄存器干预
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_LEH - 32))); delayMicroseconds(1);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_LEH - 32))); delayMicroseconds(1);
        
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(1);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(1);
    }
}

void init_screen_m0() {
    Serial.println("🔄 DMA 全屏冲洗开始...");
    set_background(0x5555); // 全黑
    for (int i=0; i<3; i++) push_frame();
    set_background(0xAAAA); // 全白
    for (int i=0; i<3; i++) push_frame();
    Serial.println("✅ 洗屏结束！");
}

// ==========================================
// 5. 启动与主循环
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    // 分配 PSRAM
    frame_buffer = (uint16_t*)heap_caps_malloc(EPD_HEIGHT * EPD_BYTES_PER_LINE * 2, MALLOC_CAP_SPIRAM);
    py_buffer = (uint8_t*)heap_caps_malloc(VIDEO_PAYLOAD_SIZE, MALLOC_CAP_SPIRAM);
    if (!frame_buffer || !py_buffer) {
        Serial.println("❌ PSRAM 分配失败！");
        while(1) delay(100);
    }
    
    // 初始化背景为全白，防止越界区域花屏
    set_background(0xAAAA);
    init_lut();

    // ==========================================
    // 彻底解决 16-bit 初始化报错的核心配置
    // ==========================================
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = PIN_STH,      // 神来之笔：把 STH 接入为 DC (数据/命令) 引脚
        .wr_gpio_num = PIN_CKH,      // 把像素时钟接入 WR (写信号)
        .clk_src = LCD_CLK_SRC_PLL160M,
        .data_gpio_nums = {
            // 严格对齐硬件 16-bit 数据线，绝不多填！
            DATA_PINS[0], DATA_PINS[1], DATA_PINS[2], DATA_PINS[3],
            DATA_PINS[4], DATA_PINS[5], DATA_PINS[6], DATA_PINS[7],
            DATA_PINS[8], DATA_PINS[9], DATA_PINS[10], DATA_PINS[11],
            DATA_PINS[12], DATA_PINS[13], DATA_PINS[14], DATA_PINS[15]
        },
        .bus_width = 16,
        .max_transfer_bytes = EPD_BYTES_PER_LINE * 2 + 8, // 每行拆分传输，释放内存压力
    };
    esp_lcd_new_i80_bus(&bus_config, &i80_bus);

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = -1,           // 不使用片选
        .pclk_hz = 6000000,         // 高达 10MHz 的 DMA 时钟！
        .trans_queue_depth = 4,
        .on_color_trans_done = color_trans_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = 16,          // 1个 cmd (即 STH 拉高 1个时钟)
        .lcd_param_bits = 16,        // 233个 data (即 STH 拉低 233个时钟)
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 1,       // 重点：发 cmd 时 STH 为高电平
            .dc_dummy_level = 0,
            .dc_data_level = 0,      // 重点：发 data 时 STH 为低电平
        }
    };
    esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle);

    // 把剩下的需要 CPU 控制的线设为普通 GPIO 输出
    pinMode(PIN_STV, OUTPUT); pinMode(PIN_CKV, OUTPUT); pinMode(PIN_LEH, OUTPUT);
    digitalWrite(PIN_STV, LOW); digitalWrite(PIN_CKV, LOW); digitalWrite(PIN_LEH, LOW);

    Wire.begin(PIN_SDA, PIN_SCL); 
    Wire.setClock(100000);
    init_pca9555();
    power_on_tps65185();
    delay(200);
    init_screen_m0(); 

    WiFi.begin(ssid, password);
    Serial.print("📡 WiFi Connecting ");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n✅ IP: %s\n", WiFi.localIP().toString().c_str());

    server.begin();
    server.setNoDelay(true);
}

void loop() {
    WiFiClient client = server.available();
    if (client) {
        client.setNoDelay(true);
        Serial.println("💻 电脑接入！");
        const char* magic = "FRAMEINC";
        
        while (client.connected()) {
            int match_idx = 0;
            while(match_idx < 8 && client.connected()) {
                if(client.available()) {
                    char c = client.read();
                    match_idx = (c == magic[match_idx]) ? match_idx + 1 : ((c == magic[0]) ? 1 : 0);
                } else { delay(1); }
            }
            
            if(match_idx == 8) {
                size_t bytes_read = 0;
                uint32_t last_time = millis();
                
                while (bytes_read < VIDEO_PAYLOAD_SIZE && client.connected()) {
                    int available = client.available();
                    if (available > 0) {
                        int to_read = min((int)(VIDEO_PAYLOAD_SIZE - bytes_read), available);
                        int read_len = client.read(py_buffer + bytes_read, to_read);
                        if (read_len > 0) bytes_read += read_len;
                        last_time = millis(); 
                    } else {
                        if (millis() - last_time > 2000) break;
                    }
                }

                if (bytes_read == VIDEO_PAYLOAD_SIZE) {
                    // 1. 数据映射进帧缓冲中心
                    update_video_region(py_buffer);
                    
                    // 2. 混合动力，火力全开
                    for(int i = 0; i < M1_PHASES; i++) push_frame();
                    
                    client.write("ACK"); 
                }
            }
        }
        Serial.println("🔌 断开连接。");
    }
}