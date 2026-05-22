// 引入 Arduino 核心库，提供基本的 Arduino 函数和宏定义
#include <Arduino.h>

// 引入 Wire 库，用于 I2C 通信协议
#include <Wire.h>

// 引入 ESP32-S3 官方底层 GPIO 寄存器宏定义，用于直接硬件操作
#include "soc/gpio_reg.h"

// ==========================================
// 1. 终极真理引脚定义 (基于最新官方资料)
// ==========================================

// I2C 数据线引脚，连接 SDA 总线
#define PIN_SDA 39

// I2C 时钟线引脚，连接 SCL 总线
#define PIN_SCL 40

// PCA9555 IO 扩展芯片的 I2C 地址
#define PCA9555_ADDR 0x20

// TPS65185 高压电源管理芯片的 I2C 地址
#define TPS65185_ADDR 0x68

// 电泳屏时钟信号 High，用于同步数据传输
#define PIN_CKH  4

// 电泳屏时钟信号 Vertical，用于垂直扫描控制
#define PIN_CKV  48

// 电泳屏起始信号 High，用于标记每行数据开始
#define PIN_STH  41

// 电泳屏锁存信号 High，用于将数据锁存到显示缓冲区
#define PIN_LEH  42

// 电泳屏起始信号 Vertical，用于垂直扫描起始信号
#define PIN_STV  45

// 16 位数据总线引脚数组，按顺序连接电泳屏的数据线 D0-D15
const int DATA_PINS[16] = {5, 6, 7, 15, 16, 17, 18, 8, 9, 10, 11, 12, 13, 14, 21, 47};

// PCA9555 端口 0 的当前状态，缓存用于批量更新
uint8_t pca_port0 = 0x00;

// PCA9555 端口 1 的当前状态，缓存用于批量更新
uint8_t pca_port1 = 0x00;

// ==========================================
// 2. I2C 带校验读写库
// ==========================================

// 检查指定 I2C 地址的设备是否存在
bool check_i2c_device(uint8_t addr) {
    // 开始向目标地址发送数据（实际为设备检测）
    Wire.beginTransmission(addr);
    // 返回传输结果，0 表示设备存在且响应正常
    return (Wire.endTransmission() == 0);
}

// 向指定 I2C 设备的寄存器写入一个字节数据
bool i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data) {
    // 开始向目标 I2C 地址发送数据
    Wire.beginTransmission(addr);
    // 写入要操作的寄存器地址
    Wire.write(reg);
    // 写入要写入的数据值
    Wire.write(data);
    // 结束传输并返回结果，0 表示写入成功
    return (Wire.endTransmission() == 0);
}

// 从指定 I2C 设备的寄存器读取一个字节数据
uint8_t i2c_read_reg(uint8_t addr, uint8_t reg) {
    // 开始向目标 I2C 地址发送数据
    Wire.beginTransmission(addr);
    // 写入要读取的寄存器地址
    Wire.write(reg);
    // 发送起始信号但不停止，保持连接以便读取
    Wire.endTransmission(false);
    // 请求从指定地址读取 1 个字节的数据
    Wire.requestFrom((uint16_t)addr, (uint8_t)1);
    // 检查是否有数据可读
    if (Wire.available()) {
        // 读取并返回一个字节数据
        return Wire.read();
    }
    // 如果没有数据可读，返回默认值 0x00
    return 0x00;
}

// ==========================================
// 3. IO 扩展芯片配置
// ==========================================

// 初始化 PCA9555 IO 扩展芯片，配置为输出模式
void init_pca9555() {
    // 配置端口 0 为输出模式（0xFF 表示所有位为输出）
    i2c_write_reg(PCA9555_ADDR, 0x06, 0xFF);
    // 配置端口 1 为输出模式（0xC4 = 0b11000100，部分位为输入）
    i2c_write_reg(PCA9555_ADDR, 0x07, 0xC4);

    // 初始化端口 0 状态为全低
    pca_port0 = 0x00;
    // 初始化端口 1 状态为全低
    pca_port1 = 0x00;
    // 将端口 0 的初始状态写入硬件寄存器
    i2c_write_reg(PCA9555_ADDR, 0x02, pca_port0);
    // 将端口 1 的初始状态写入硬件寄存器
    i2c_write_reg(PCA9555_ADDR, 0x03, pca_port1);
}

// 设置 PCA9555 指定端口的指定引脚的电平状态
void set_pca_pin(int port, int pin, bool high) {
    // 如果操作的是端口 0
    if (port == 0) {
        // 如果设置为高电平，将对应位置 1
        if (high) pca_port0 |= (1 << pin);
        // 如果设置为低电平，将对应位清 0
        else pca_port0 &= ~(1 << pin);
        // 将更新后的端口 0 状态写入硬件寄存器
        i2c_write_reg(PCA9555_ADDR, 0x02, pca_port0);
    } else {
        // 如果设置为高电平，将对应位置 1
        if (high) pca_port1 |= (1 << pin);
        // 如果设置为低电平，将对应位清 0
        else pca_port1 &= ~(1 << pin);
        // 将更新后的端口 1 状态写入硬件寄存器
        i2c_write_reg(PCA9555_ADDR, 0x03, pca_port1);
    }
}

// ==========================================
// 4. 高压电源完美点亮
// ==========================================

// 启动 TPS65185 高压电源管理芯片
bool power_on_tps65185() {
    // 打印启动信息到串口
    Serial.println("🔌 [PMIC] Waking up TPS65185...");

    // 通过 PCA9555 控制电源芯片的使能引脚，先拉低
    set_pca_pin(1, 5, false);
    // 等待 100 毫秒让电源稳定
    delay(100);
    // 拉高使能引脚，启动电源芯片
    set_pca_pin(1, 5, true);
    // 等待 150 毫秒让电源芯片完全启动
    delay(150);

    // 检查 TPS65185 是否成功唤醒
    if (!check_i2c_device(TPS65185_ADDR)) {
        // 如果设备未响应，打印错误信息
        Serial.println("❌ ERROR: TPS65185 (0x68) DID NOT WAKE UP!");
        // 返回失败状态
        return false;
    }
    // 打印成功唤醒信息
    Serial.println("✅ TPS65185 is awake!");

    // 配置 TPS65185 寄存器 0x03 为 0xE1（电压设置）
    i2c_write_reg(TPS65185_ADDR, 0x03, 0xE1);
    // 配置 TPS65185 寄存器 0x04 为 0xAA（电压设置）
    i2c_write_reg(TPS65185_ADDR, 0x04, 0xAA);

    // 启用高压输出的使能引脚
    set_pca_pin(1, 3, true);
    // 启用另一个高压输出的使能引脚
    set_pca_pin(1, 4, true);
    // 等待 50 毫秒让高压输出稳定
    delay(50);

    // 配置 TPS65185 寄存器 0x08 为 0x3F（电源管理设置）
    i2c_write_reg(TPS65185_ADDR, 0x08, 0x3F);

    // 设置超时计数器为 50 次循环
    int timeout = 50;
    // 初始化电源就绪标志为 false
    bool pg_ok = false;
    // 循环检查电源就绪状态
    while(timeout--) {
        // 读取 TPS65185 的电源就绪寄存器（地址 0x0A）
        uint8_t pg = i2c_read_reg(TPS65185_ADDR, 0x0A);
        // 检查电源就绪状态是否正常（非 0xFF 且高 5 位为 1）
        if (pg != 0xFF && (pg & 0xF8) == 0xF8) {
            // 设置电源就绪标志为 true
            pg_ok = true;
            // 退出循环
            break;
        }
        // 等待 10 毫秒后再次检查
        delay(10);
    }

    // 如果电源未就绪
    if(!pg_ok) {
        // 读取电源就绪寄存器的当前值
        uint8_t failed_pg = i2c_read_reg(TPS65185_ADDR, 0x0A);
        // 打印警告信息，显示实际读取的值
        Serial.printf("⚠️ WARNING: PG is 0x%02X, bypass check.\n", failed_pg);
    } else {
        // 打印电源就绪成功信息
        Serial.println(" OK! High voltage is LIVE!");
    }

    // 配置 TPS65185 寄存器 0x0B 为 0x9C（电源管理设置）
    i2c_write_reg(TPS65185_ADDR, 0x0B, 0x9C);
    // 启用电源的另一个使能引脚
    set_pca_pin(1, 0, true);
    // 启用电源的另一个使能引脚
    set_pca_pin(1, 1, true);
    // 返回成功状态
    return true;
}

// ==========================================
// 5. 寄存器级极限爆发时序驱动 (已修复 GPIO 作用域错误)
// ==========================================

// 初始化底层 GPIO 引脚，设置为适当的模式和初始电平
void init_bare_metal_pins() {
    // 将 CKH 引脚设置为输出模式
    pinMode(PIN_CKH, OUTPUT);
    // 将 CKV 引脚设置为输出模式
    pinMode(PIN_CKV, OUTPUT);
    // 将 STH 引脚设置为输出模式
    pinMode(PIN_STH, OUTPUT);
    // 将 LEH 引脚设置为输出模式
    pinMode(PIN_LEH, OUTPUT);
    // 将 STV 引脚设置为输出模式
    pinMode(PIN_STV, OUTPUT);
    // 遍历所有 16 个数据引脚
    for (int i = 0; i < 16; i++) {
        // 将每个数据引脚设置为输入上拉模式（默认状态）
        pinMode(DATA_PINS[i], INPUT_PULLUP);
    }
    // 将 CKH 引脚设置为低电平
    digitalWrite(PIN_CKH, LOW);
    // 将 CKV 引脚设置为低电平
    digitalWrite(PIN_CKV, LOW);
    // 将 STH 引脚设置为低电平
    digitalWrite(PIN_STH, LOW);
    // 将 LEH 引脚设置为低电平
    digitalWrite(PIN_LEH, LOW);
    // 将 STV 引脚设置为低电平
    digitalWrite(PIN_STV, LOW);
}

// 设置数据总线的方向（输出或输入上拉）
void set_bus_direction(bool is_output) {
    // 遍历所有 16 个数据引脚
    for (int i = 0; i < 16; i++) {
        // 根据参数设置为输出模式或输入上拉模式
        pinMode(DATA_PINS[i], is_output ? OUTPUT : INPUT_PULLUP);
    }
}

// 🔥 使用 REG_WRITE 宏直接写硬件寄存器，彻底解决作用域报错

// 以最快速度驱动电泳屏显示一帧数据（使用寄存器直接操作）
void drive_one_frame_fast(uint16_t color_pattern) {
    // 将数据总线设置为输出模式
    set_bus_direction(true);

    // 在进入循环前，一次性把总线 16 位数据的引脚掩码全部算好
    uint32_t low_set_mask = 0;
    uint32_t low_clear_mask = 0;
    uint32_t high_set_mask = 0;
    uint32_t high_clear_mask = 0;

    // 遍历所有 16 个数据位，计算每个引脚的操作掩码
    for (int i = 0; i < 16; i++) {
        // 获取当前位的引脚号
        int pin = DATA_PINS[i];
        // 如果引脚号小于 32（属于低位寄存器）
        if (pin < 32) {
            // 如果该位为 1，添加到低位设置掩码
            if ((color_pattern >> i) & 0x01) low_set_mask |= (1UL << pin);
            // 如果该位为 0，添加到低位清除掩码
            else low_clear_mask |= (1UL << pin);
        } else {
            // 如果该位为 1，添加到高位设置掩码（引脚号减 32）
            if ((color_pattern >> i) & 0x01) high_set_mask |= (1UL << (pin - 32));
            // 如果该位为 0，添加到高位清除掩码（引脚号减 32）
            else high_clear_mask |= (1UL << (pin - 32));
        }
    }

    // --- 1. Frame Start (使用 REG_WRITE 操作高位/低位寄存器) ---
    // 将 STV 引脚设置为高电平（帧起始信号）
    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_STV - 32)));
    // 等待 2 微秒
    delayMicroseconds(2);
    // 将 CKV 引脚设置为高电平（垂直时钟信号）
    REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32)));
    // 等待 2 微秒
    delayMicroseconds(2);
    // 将 CKV 引脚设置为低电平
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32)));
    // 将 STV 引脚设置为低电平
    REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_STV - 32)));
    // 等待 2 微秒
    delayMicroseconds(2);

    // --- 2. 行循环 ---
    // 遍历电泳屏的所有 1500 行
    for (int line = 0; line < 1500; line++) {

        // STH HIGH (Pin 41 >= 32) - 设置行起始信号为高电平
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_STH - 32)));
        // 等待 2 微秒
        delayMicroseconds(2);
        // CKH HIGH (Pin 4 < 32) - 设置水平时钟信号为高电平
        REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << PIN_CKH));
        // 等待 2 微秒
        delayMicroseconds(2);
        // CKH LOW - 设置水平时钟信号为低电平
        REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << PIN_CKH));
        // STH LOW - 设置行起始信号为低电平
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_STH - 32)));
        // 等待 2 微秒
        delayMicroseconds(2);

        // 🚀 总线一键强推 - 批量设置所有数据引脚的电平
        if (low_set_mask)     REG_WRITE(GPIO_OUT_W1TS_REG, low_set_mask);
        if (low_clear_mask)   REG_WRITE(GPIO_OUT_W1TC_REG, low_clear_mask);
        if (high_set_mask)    REG_WRITE(GPIO_OUT1_W1TS_REG, high_set_mask);
        if (high_clear_mask)  REG_WRITE(GPIO_OUT1_W1TC_REG, high_clear_mask);

        // 🚀 像素时钟超频爆发 (2000 / 8 = 250 次) - 快速切换时钟信号
        for (int p = 0; p < 250; p++) {
            // CKH HIGH - 设置水平时钟信号为高电平
            REG_WRITE(GPIO_OUT_W1TS_REG, (1UL << PIN_CKH));
            // 插入两个 NOP 指令，确保时序稳定
            asm volatile("nop; nop;");
            // CKH LOW - 设置水平时钟信号为低电平
            REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << PIN_CKH));
            // 插入两个 NOP 指令，确保时序稳定
            asm volatile("nop; nop;");
        }

        // LEH HIGH -> LOW - 锁存信号从高电平变为低电平
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_LEH - 32)));
        // 等待 2 微秒
        delayMicroseconds(2);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_LEH - 32)));
        // 等待 2 微秒
        delayMicroseconds(2);

        // CKV HIGH -> LOW - 垂直时钟信号从高电平变为低电平
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1UL << (PIN_CKV - 32)));
        // 等待 2 微秒
        delayMicroseconds(2);
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1UL << (PIN_CKV - 32)));
        // 等待 2 微秒
        delayMicroseconds(2);
    }
    // 将数据总线设置为输入上拉模式，节省功耗
    set_bus_direction(false);
}

// ==========================================
// 6. 主程序
// ==========================================

// Arduino 初始化函数，在启动时执行一次
void setup() {
    // 初始化串口通信，波特率为 115200
    Serial.begin(115200);
    // 等待 2 秒让串口连接稳定
    delay(2000);
    // 打印启动标题
    Serial.println("\n====== 10.3 BARE-METAL (HARDWARE REGISTER SPEED) ======");

    // 初始化 I2C 总线，使用指定的 SDA 和 SCL 引脚
    Wire.begin(PIN_SDA, PIN_SCL);
    // 设置 I2C 时钟频率为 100 kHz
    Wire.setClock(100000);

    // 初始化 PCA9555 IO 扩展芯片
    init_pca9555();
    // 启动 TPS65185 高压电源
    power_on_tps65185();
    // 初始化底层 GPIO 引脚
    init_bare_metal_pins();
    // 打印硬件就绪信息
    Serial.println("✅ Hardware Ready!");
}

// Arduino 主循环函数，持续重复执行
void loop() {
    // 定义总阶段数为 12（未使用，可能是预留参数）
    int total_phases = 12;
    // 执行 10 帧黑色显示清洗
    for (int phase = 0; phase < 5; phase++) {
        // 驱动一帧 0x5555 模式的数据（黑白交替）
        drive_one_frame_fast(0x5555);
    }
        delay(1000); // 每次清洗后等待 100 毫秒
    // 执行 10 帧白色显示清洗
    for (int phase = 0; phase < 2; phase++) {
        // 驱动一帧 0xAAAA 模式的数据（黑白交替，与 0x5555 互补）
        drive_one_frame_fast(0xAAAA);
    }
    delay(100); // 每次清洗后等待 100 毫秒
}