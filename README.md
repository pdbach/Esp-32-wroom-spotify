ESP32 Music Player

ESP32 Music Player là một dự án sử dụng ESP32-WROOM, màn hình ST7735 1.44", và loa I2S MAX98357A để phát nhạc qua Bluetooth A2DP hoặc Spotify (qua WiFi/SIM 4G).

Tính năng

Hai chế độ phát nhạc:

Bluetooth A2DP -> phát nhạc từ điện thoại.

Spotify -> phát nhạc online qua WiFi hoặc SIM 4G.

Điều khiển bằng 3 nút bấm: LEFT, RIGHT, OK.

Hiển thị thông tin trên màn hình ST7735 (bài hát, trạng thái, menu).

Hỗ trợ I2S audio với MAX98357A.

Hỗ trợ SIM 4G (với TinyGSM).

Kết nối phần cứng
Thiết bị	Chân kết nối
ST7735	CS: 5, DC: 2, RST: 4
Nút OK	15
Nút LEFT	32
Nút RIGHT	33
MAX98357A	BCLK: 26, LRC: 25, DIN: 22
SIM 4G	RX: 16, TX: 17
Cài đặt

Cài đặt thư viện trong Arduino IDE:

Adafruit_GFX

Adafruit_ST7735

BluetoothA2DPSink

TinyGSM

Nạp code ESP32_MusicPlayer_Loop.ino vào ESP32.

Kết nối phần cứng theo sơ đồ trên.

Sử dụng

Khi khởi động, chọn chế độ phát nhạc bằng nút LEFT/RIGHT, xác nhận bằng nút OK.

Nếu chọn Spotify, tiếp tục chọn mạng WiFi hoặc SIM.

Khi vào chế độ phát nhạc, nút LEFT/RIGHT/OK điều khiển bài hát.

Lưu ý

Code không hỗ trợ hiển thị tiếng Việt để tránh lỗi font.

Sử dụng SIM 4G cần đăng ký gói dữ liệu.

Tối ưu cho màn hình 1.44" và loa MAX98357A.
