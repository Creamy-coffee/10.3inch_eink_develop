#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "soc/gpio_reg.h"

// ==========================================
// 0. 网络与分辨率配置
// ==========================================
const char* ssid = "PCDN1";
const char* password = "431227200";
const int tcp_port = 8080;
WiFiServer server(tcp_port);

#define EPD_WIDTH 1872
#define EPD_HEIGHT 1404
#define EPD_BYTES_PER_LINE (EPD_WIDTH / 8)           // 234 Bytes
#define FRAME_SIZE (EPD_BYTES_PER_LINE * EPD_HEIGHT) // 328,536 Bytes

#define M1_PHASES 2 

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

uint8_t pca_port0 = 0x00;
uint8_t pca_port1 = 0x00;

// ==========================================
// 1. 三缓冲与 FreeRTOS 队列设计 (核心引擎)
// ==========================================
uint8_t* buf0 = nullptr;
uint8_t* buf1 = nullptr;
uint8_t* buf2 = nullptr;

uint8_t* ptr_glass = nullptr; // 当前屏幕上显示的画面

// 传递缓冲区指针的队列
QueueHandle_t empty_queue; // 存放空闲缓冲区的指针，供网络接收使用
QueueHandle_t ready_queue; // 存放已填满画面的指针，供屏幕刷新使用

// 内部极速 SRAM 行缓冲
uint32_t low_set_line[EPD_BYTES_PER_LINE];
uint32_t low_clear_line[EPD_BYTES_PER_LINE];
uint32_t high_set_line[EPD_BYTES_PER_LINE];
uint32_t high_clear_line[EPD_BYTES_PER_LINE];

// 256级 超高速差分 LUT 表
uint32_t lut_ls_hi[256], lut_lc_hi[256], lut_hs_hi[256], lut_hc_hi[256];
uint32_t lut_ls_lo[256], lut_lc_lo[256], lut_hs_lo[256], lut_hc_lo[256];

// ==========================================
// 2. 超高速 LUT 与 硬件初始化
// ==========================================
void build_fast_lut() {
    Serial.println("⚙️ Compiling SRAM LUT...");
    for(int idx = 0; idx < 256; idx++) {
        uint8_t old_nib = idx >> 4;
        uint8_t new_nib = idx & 0x0F;
        
        uint32_t ls_hi=0, lc_hi=0, hs_hi=0, hc_hi=0;
        uint32_t ls_lo=0, lc_lo=0, hs_lo=0, hc_lo=0;

        for(int p=0; p<4; p++) {
            bool o_bit = (old_nib & (1 << (3-p))) != 0; 
            bool n_bit = (new_nib & (1 << (3-p))) != 0;
            int pin_idx = p * 2; 
            int p0 = DATA_PINS[pin_idx];
            int p1 = DATA_PINS[pin_idx + 1];
            bool bus0 = false, bus1 = false;
            
            if (o_bit != n_bit) {
                if (n_bit) { bus1 = true; bus0 = false; } 
                else       { bus1 = false; bus0 = true; } 
            }
            if (p0 < 32) { if(bus0) ls_hi |= (1UL<<p0); else lc_hi |= (1UL<<p0); }
            else         { if(bus0) hs_hi |= (1UL<<(p0-32)); else hc_hi |= (1UL<<(p0-32)); }
            if (p1 < 32) { if(bus1) ls_hi |= (1UL<<p1); else lc_hi |= (1UL<<p1); }
            else         { if(bus1) hs_hi |= (1UL<<(p1-32)); else hc_hi |= (1UL<<(p1-32)); }
        }
        
        for(int p=0; p<4; p++) {
            bool o_bit = (old_nib & (1 << (3-p))) != 0;
            bool n_bit = (new_nib & (1 << (3-p))) != 0;
            int pin_idx = (p + 4) * 2; 
            int p0 = DATA_PINS[pin_idx];
            int p1 = DATA_PINS[pin_idx + 1];
            bool bus0 = false, bus1 = false;
            
            if (o_bit != n_bit) {
                if (n_bit) { bus1 = true; bus0 = false; }
                else       { bus1 = false; bus0 = true; }
            }
            if (p0 < 32) { if(bus0) ls_lo |= (1UL<<p0); else lc_lo |= (1UL<<p0); }
            else         { if(bus0) hs_lo |= (1UL<<(p0-32)); else hc_lo |= (1UL<<(p0-32)); }
            if (p1 < 32) { if(bus1) ls_lo |= (1UL<<p1); else lc_lo |= (1UL<<p1); }
            else         { if(bus1) hs_lo |= (1UL<<(p1-32)); else hc_lo |= (1UL<<(p1-32)); }
        }
        
        lut_ls_hi[idx] = ls_hi; lut_lc_hi[idx] = lc_hi; lut_hs_hi[idx] = hs_hi; lut_hc_hi[idx] = hc_hi;
        lut_ls_lo[idx] = ls_lo; lut_lc_lo[idx] = lc_lo; lut_hs_lo[idx] = hs_lo; lut_hc_lo[idx] = hc_lo;
    }
}

bool i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data) {
    Wire.beginTransmission(addr); Wire.write(reg); Wire.write(data);
    return (Wire.endTransmission() == 0);
}
uint8_t i2c_read_reg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr); Wire.write(reg); Wire.endTransmission(false);
    Wire.requestFrom((uint16_t)addr, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return 0x00;
}

void init_pca9555() {
    i2c_write_reg(PCA9555_ADDR, 0x06, 0xFF); i2c_write_reg(PCA9555_ADDR, 0x07, 0xC4);
    pca_port0 = 0x00; pca_port1 = 0x00;
    i2c_write_reg(PCA9555_ADDR, 0x02, pca_port0); i2c_write_reg(PCA9555_ADDR, 0x03, pca_port1);
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

void power_on_tps65185() {
    Serial.println("🔌 启动高压电源...");
    set_pca_pin(1, 5, false); delay(100);
    set_pca_pin(1, 5, true); delay(200);
    i2c_write_reg(TPS65185_ADDR, 0x03, 0xE1); i2c_write_reg(TPS65185_ADDR, 0x04, 0xAA);
    set_pca_pin(1, 3, true); set_pca_pin(1, 4, true); delay(100);
    i2c_write_reg(TPS65185_ADDR, 0x08, 0x3F); i2c_write_reg(TPS65185_ADDR, 0x0B, 0x9C);
    set_pca_pin(1, 0, true); set_pca_pin(1, 1, true); delay(100);
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
// 3. M1 极限底层驱动 (修改为接收指针)
// ==========================================
void drive_image_diff(uint8_t* p_old, uint8_t* p_new) {
    set_bus_direction(true);

    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_STV - 32))); delayMicroseconds(3);
    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(3);
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32)));
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_STV - 32))); delayMicroseconds(3);

    int ptr = 0;
    for (int line = 0; line < EPD_HEIGHT; line++) {
        for(int p = 0; p < EPD_BYTES_PER_LINE; p++) {
            uint8_t ob = p_old[ptr];
            uint8_t nb = p_new[ptr];
            ptr++;
            
            uint8_t idx_hi = (ob & 0xF0) | (nb >> 4);
            uint8_t idx_lo = ((ob & 0x0F) << 4) | (nb & 0x0F);
            
            low_set_line[p]   = lut_ls_hi[idx_hi] | lut_ls_lo[idx_lo];
            low_clear_line[p] = lut_lc_hi[idx_hi] | lut_lc_lo[idx_lo];
            high_set_line[p]  = lut_hs_hi[idx_hi] | lut_hs_lo[idx_lo];
            high_clear_line[p]= lut_hc_hi[idx_hi] | lut_hc_lo[idx_lo];
        }

        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_STH - 32))); delayMicroseconds(3);
        REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << PIN_CKH)); delayMicroseconds(3);
        REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << PIN_CKH));
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_STH - 32))); delayMicroseconds(3);

        for (int p = 0; p < EPD_BYTES_PER_LINE; p++) {
            REG_WRITE(GPIO_OUT_W1TS_REG, low_set_line[p]);
            REG_WRITE(GPIO_OUT_W1TC_REG, low_clear_line[p]);
            REG_WRITE(GPIO_OUT1_W1TS_REG, high_set_line[p]);
            REG_WRITE(GPIO_OUT1_W1TC_REG, high_clear_line[p]);

            REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << PIN_CKH));
            asm volatile("nop;nop;nop;nop;nop;"); 
            REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << PIN_CKH));
            asm volatile("nop;nop;nop;nop;nop;");
        }

        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_LEH - 32))); delayMicroseconds(3);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_LEH - 32))); delayMicroseconds(3);
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(3);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32))); delayMicroseconds(3);
    }
    set_bus_direction(false);
}

void init_screen_m0() {
    Serial.println("🔄 [M0 Init] 洗屏开始...");
    memset(buf0, 0xFF, FRAME_SIZE); 
    memset(buf1, 0x00, FRAME_SIZE); 
    for (int i=0; i<3; i++) drive_image_diff(buf0, buf1);
    
    memset(buf0, 0x00, FRAME_SIZE);
    memset(buf1, 0xFF, FRAME_SIZE);
    for (int i=0; i<3; i++) drive_image_diff(buf0, buf1);
    
    // 初始化时，让屏幕保持白底
    memset(ptr_glass, 0xFF, FRAME_SIZE);
    Serial.println("✅ [M0 Init] 洗屏结束！");
}

// ==========================================
// 4. FreeRTOS 双核任务分配
// ==========================================

// 任务1 (运行在 Core 0): 专门负责网络接收，极度贪婪地读取 TCP
void networkTask(void *pvParameters) {
    server.begin();
    server.setNoDelay(true);
    Serial.println("🎯 网络任务 (Core 0) 就绪！");

    const char* magic = "FRAMEINC";
    
    for (;;) {
        WiFiClient client = server.available();
        if (client) {
            client.setNoDelay(true);
            Serial.println("💻 电脑接入，开始疯狂接收...");
            
            while (client.connected()) {
                int match_idx = 0;
                while(match_idx < 8 && client.connected()) {
                    if(client.available()) {
                        char c = client.read();
                        match_idx = (c == magic[match_idx]) ? match_idx + 1 : ((c == magic[0]) ? 1 : 0);
                    } else { vTaskDelay(1); }
                }
                
                if(match_idx == 8) {
                    uint8_t* p_recv = nullptr;
                    // 从空闲队列拿一个 buffer (如果满了就等屏幕腾出空间)
                    xQueueReceive(empty_queue, &p_recv, portMAX_DELAY);

                    size_t bytes_read = 0;
                    uint32_t last_time = millis();
                    
                    while (bytes_read < FRAME_SIZE && client.connected()) {
                        int available = client.available();
                        if (available > 0) {
                            int to_read = min((int)(FRAME_SIZE - bytes_read), available);
                            int read_len = client.read(p_recv + bytes_read, to_read);
                            if (read_len > 0) bytes_read += read_len;
                            last_time = millis(); 
                        } else {
                            if (millis() - last_time > 2000) break;
                            vTaskDelay(1); // 释放一点算力
                        }
                    }

                    if (bytes_read == FRAME_SIZE) {
                        // 收满一帧，把指针丢给屏幕任务去刷
                        xQueueSend(ready_queue, &p_recv, portMAX_DELAY);
                        client.write("ACK"); 
                    } else {
                        // 接收失败，把 buffer 还给空闲队列
                        xQueueSend(empty_queue, &p_recv, portMAX_DELAY);
                    }
                }
            }
            Serial.println("🔌 连接断开。");
        }
        vTaskDelay(10); 
    }
}

// 任务2 (运行在 Core 1): 专门负责驱动墨水屏，完全不被网络阻塞
void displayTask(void *pvParameters) {
    Serial.println("📺 显示任务 (Core 1) 就绪！");
    for (;;) {
        uint8_t* p_draw = nullptr;
        // 阻塞等待网络发来填满的画面
        if (xQueueReceive(ready_queue, &p_draw, portMAX_DELAY) == pdTRUE) {
            
            // 计算屏幕现有画面(ptr_glass)和新画面(p_draw)的差异，刷出去
            for(int i = 0; i < M1_PHASES; i++) {
                drive_image_diff(ptr_glass, p_draw);
            }
            
            // 刷完了，把旧的玻璃画面指针扔回给网络去覆盖
            xQueueSend(empty_queue, &ptr_glass, portMAX_DELAY);
            // 现在的玻璃画面变成了刚刚画的这帧
            ptr_glass = p_draw; 
        }
    }
}

// ==========================================
// 5. 启动程序
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    // 在 PSRAM 开辟 3 个缓冲区 (约 1MB)
    buf0 = (uint8_t*)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    buf1 = (uint8_t*)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    buf2 = (uint8_t*)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    
    if (!buf0 || !buf1 || !buf2) {
        Serial.println("❌ OPI PSRAM 分配失败！");
        while(1) delay(1000);
    }

    // 初始化队列：存放指针，容量为 3
    empty_queue = xQueueCreate(3, sizeof(uint8_t*));
    ready_queue = xQueueCreate(3, sizeof(uint8_t*));

    ptr_glass = buf0;
    // 启动时，把另外两个 buffer 扔给网络去用
    xQueueSend(empty_queue, &buf1, portMAX_DELAY);
    xQueueSend(empty_queue, &buf2, portMAX_DELAY);

    build_fast_lut();
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
    init_pca9555();
    power_on_tps65185();
    init_bare_metal_pins();
    init_screen_m0(); 

    WiFi.begin(ssid, password);
    Serial.print("📡 WiFi 正在连接 ");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n✅ IP: %s\n", WiFi.localIP().toString().c_str());

    // 绑定网络任务到 Core 0
    xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 2, NULL, 0);
    // 绑定显示任务到 Core 1
    xTaskCreatePinnedToCore(displayTask, "DisplayTask", 8192, NULL, 3, NULL, 1);
}

void loop() {
    // 主循环彻底闲置，把所有算力交给 RTOS 任务
    vTaskDelete(NULL);
}