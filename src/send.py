import cv2
import numpy as np
import socket
import time

# ==========================================
# 配置参数
# ==========================================
VIDEO_PATH = "badapple.mp4"
ESP32_IP = "192.168.137.133"  # 替换为串口打印出的 ESP32 IP地址
ESP32_PORT = 8080

EPD_WIDTH = 2000
EPD_HEIGHT = 1500


def main():
    print(f"📡 正在连接 ESP32 ({ESP32_IP}:{ESP32_PORT})...")
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        client.connect((ESP32_IP, ESP32_PORT))
        print("✅ 连接成功！")
    except Exception as e:
        print(f"❌ 连接失败: {e}")
        return

    cap = cv2.VideoCapture(VIDEO_PATH)
    if not cap.isOpened():
        print(f"❌ 找不到视频文件: {VIDEO_PATH}")
        return

    frame_count = 0
    start_time = time.time()

    while True:
        ret, frame = cap.read()
        if not ret:
            print("🎬 视频播放结束。")
            break

        # 1. 调整分辨率并转为灰度图
        resized = cv2.resize(frame, (EPD_WIDTH, EPD_HEIGHT), interpolation=cv2.INTER_AREA)
        gray = cv2.cvtColor(resized, cv2.COLOR_BGR2GRAY)

        # 2. 二值化 (Otsu算法或固定阈值，这里用固定 127)
        _, thresh = cv2.threshold(gray, 127, 255, cv2.THRESH_BINARY)

        # 3. 转换成 1-bit numpy array (0 和 1)
        # OpenCV 中 255 是白色，0 是黑色。将其转为 1 和 0
        binary_array = (thresh // 255).astype(np.uint8)

        # 4. 将 8 个像素打包成 1 个 byte (极大地压缩网络传输)
        # 例如 [1, 0, 1, 1, 0, 0, 1, 0] -> 0xB2
        packed_data = np.packbits(binary_array)

        try:
            # 5. 发送数据：先发 8 字节同步头，再发 375KB 图像主体
            client.sendall(b"FRAMEINC")
            client.sendall(packed_data.tobytes())

            # 6. 等待 ESP32 刷完这帧发回的 ACK (背压流控，防止把 ESP32 内存撑爆)
            ack = client.recv(3)
            if ack != b"ACK":
                print("⚠️ 收到未知回应或超时")

        except Exception as e:
            print(f"❌ 传输中断: {e}")
            break

        frame_count += 1
        elapsed = time.time() - start_time
        fps = frame_count / elapsed
        print(f"🎞️ 已发送第 {frame_count} 帧 | 预估网络/渲染综合 FPS: {fps:.2f}", end='\r')

    cap.release()
    client.close()


if __name__ == "__main__":
    main()