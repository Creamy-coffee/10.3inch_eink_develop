import cv2
import numpy as np
import socket
import time
import sys

# ==========================================
# 核心配置参数
# ==========================================
VIDEO_PATH = "badapple.mp4"
ESP32_IP = "192.168.137.29"  # ★★★ 请务必修改为你单片机打印的 IP ★★★
ESP32_PORT = 8080

# 严格锁定原始视频核心大小
VIDEO_WIDTH = 1440
VIDEO_HEIGHT = 1080
EXPECTED_SIZE = int(VIDEO_WIDTH * VIDEO_HEIGHT / 8)  # 194400 字节


def main():
    print(f"📡 正在连接 DMA 硬件引擎 ({ESP32_IP}:{ESP32_PORT})...")
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    client.settimeout(2.0)

    try:
        client.connect((ESP32_IP, ESP32_PORT))
        print("✅ 连接成功，准备注入 1:1 原始流！")
    except Exception as e:
        print(f"❌ 连接失败: {e}")
        return

    cap = cv2.VideoCapture(VIDEO_PATH)
    if not cap.isOpened():
        print(f"❌ 找不到视频文件: {VIDEO_PATH}")
        return

    frame_count = 0
    prev_time = time.time()
    fps_smooth = 0.0

    while True:
        ret, frame = cap.read()
        if not ret:
            print("\n🎬 播放完美结束！")
            break

        # 1. 强行锁定 1440x1080，防视频源尺寸异变，确保大小极其严格
        frame = cv2.resize(frame, (VIDEO_WIDTH, VIDEO_HEIGHT))

        # 2. 灰度化
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        # 3. 计算帧率，不打在视频上，保持画面绝对纯净
        curr_time = time.time()
        fps = 1.0 / (curr_time - prev_time + 0.0001)
        prev_time = curr_time
        fps_smooth = fps_smooth * 0.9 + fps * 0.1

        # 4. 一刀切阈值 (完美像素级还原，没有任何抖动和抗锯齿模糊)
        _, thresh = cv2.threshold(gray, 127, 255, cv2.THRESH_BINARY)

        # 5. 打包成 8 位传输格式
        binary_array = (thresh // 255).astype(np.uint8)
        packed_data = np.packbits(binary_array)

        payload = packed_data.tobytes()
        if len(payload) != EXPECTED_SIZE:
            print(f"❌ 尺寸异变: 期待 {EXPECTED_SIZE}, 实际 {len(payload)}")
            continue

        try:
            client.sendall(b"FRAMEINC")
            client.sendall(payload)

            # 等待 DMA 把这一帧刷完
            ack = client.recv(3)
        except socket.timeout:
            pass
        except Exception as e:
            print(f"\n❌ 网络炸裂: {e}")
            break

        frame_count += 1
        sys.stdout.write(f"\r🎞️ DMA 高速推送中... 帧: {frame_count} | 理论吞吐 FPS: {fps_smooth:.1f}")
        sys.stdout.flush()

    cap.release()
    client.close()


if __name__ == "__main__":
    main()