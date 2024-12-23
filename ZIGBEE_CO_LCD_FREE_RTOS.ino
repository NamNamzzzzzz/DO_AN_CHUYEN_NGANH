#include <HardwareSerial.h>
#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9341.h"
#include <WiFi.h>
#include "time.h"

HardwareSerial zigbeeSerial(1);

// Đám này cho LCD ILI9341
#define TFT_CS 0
#define TFT_DC 2
#define TFT_MOSI 14
#define TFT_SCLK 12
#define TFT_RST 13

//Đám này dành cho RTC ESP32
const char* ssid = "Trieu Ninh";
const char* password = "12344321";
const char* ntpServer = "time.nist.gov";
const long gmtOffset_sec = 25200;
const int daylightOffset_sec = 3600;

//Đám này cho nút nhấn và LED để điều khiển các ngoại vi sau này
#define BUTTON 26
#define BUTTON2 25
#define CLEAR_DS 34
volatile bool quat = false;
volatile bool oxi = false;
volatile bool led_state3 = false;
volatile bool th_adjust = false;

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

//Tạo một cấu trúc enum với n phần tử
enum DisplayUpdateType {UPDATE_TDS, UPDATE_WATER, UPDATE_PH, UPDATE_TIME, UPDATE_FAN, UPDATE_OXI, UPDATE_LED3};

//Tạo một struct để tổ chức "display update" theo nhóm
//Struct này ảnh hưởng trực tiếp đến thứ gì được phép cập nhật lên LCD, cũng như thứ tự của chúng
struct DisplayUpdate {
  DisplayUpdateType type;
  float tds_value;
  float water_value;
  float ph_value;
  String time_str;
};

//Tạo một hàng đợi, tránh xung đột làm trắng màn hình LCD
QueueHandle_t displayQueue;

//Tạo task handle để kiểm soát các task
TaskHandle_t received_display_data;
TaskHandle_t fan_button;
TaskHandle_t oxi_button;
TaskHandle_t display_ili;
TaskHandle_t send_time;
TaskHandle_t adjust;

//Con trỏ trỏ trực tiếp đến các biến giờ và phút để điều chỉnh thời gian 
struct tm timeinfo;
  int *hour_ptr = &timeinfo.tm_hour;
  int *min_ptr = &timeinfo.tm_min;

void setup() {
  Serial.begin(9600);
  zigbeeSerial.begin(115200, SERIAL_8N1, 16, 17);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(BUTTON2, INPUT_PULLUP);
  pinMode(CLEAR_DS, INPUT_PULLUP);

  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  //Nếu kết nối không được, in ra một đống dấu chấm
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected.");
  
  //Lấy thông tin thời gian, tốt nhất nên có delay 1 tí mắc công sever lỏ
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  delay(5000);
  //Lấy được thông tin thgian rồi thì tắt luôn wifi
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  //Các hàm khởi tạo mặc định của ILI9341
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_BLUE, ILI9341_BLACK);
  tft.setTextSize(2);

  tft.setCursor(20, 10);
  tft.println("DO AN CHUYEN NGANH");

  tft.setCursor(20, 105);
  tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
  tft.print("FAN: ");
  tft.setCursor(70, 105);
  tft.print("OFF");
  tft.setCursor(20, 130);
  tft.print("OXI: ");
  tft.setCursor(70, 130);
  tft.print("OFF");
  tft.setCursor(20, 155);
  tft.print("LED3: ");
  tft.setCursor(82, 155);
  tft.print("OFF");

  tft.setCursor(20, 200);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.print("Time: ");

  //Tạo hàng đợi
  displayQueue = xQueueCreate(8, sizeof(DisplayUpdate));

  //Tạo task cho từng công việc, riêng việc nhận data sẽ phải ghim ở core 0 vì core 0 chuyên cho việc này
  xTaskCreatePinnedToCore(receiveDisplayDataTask, "ReceiveDisplayDataTask", 8192, NULL, 2, &received_display_data, 0);
  xTaskCreatePinnedToCore(push_button, "Push_button", 4096, NULL, 1, &fan_button, 1);
  xTaskCreatePinnedToCore(push_button2, "Push_button2", 4096, NULL, 1, &oxi_button, 1);
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 8192, NULL, 1, &display_ili, 1);
  xTaskCreatePinnedToCore(send_timer, "Send_timer", 8192, NULL, 0, &send_time, 0);
  xTaskCreatePinnedToCore(adjust_time, "Adjust_time", 4096, NULL, 1, &adjust, 1);
}

void displayTask(void *pvParameters) {
  DisplayUpdate receive_update;
  while (true) {
    if (xQueueReceive(displayQueue, &receive_update, portMAX_DELAY)) {
      if (receive_update.type == UPDATE_TDS) {
        tft.fillRect(120, 30, 100, 20, ILI9341_BLACK);
        tft.setCursor(20, 30);
        tft.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
        tft.print("TDS_Val:");
        tft.print(receive_update.tds_value);
      } 
      else if (receive_update.type == UPDATE_WATER) {
        tft.fillRect(135, 55, 100, 20, ILI9341_BLACK);
        tft.setCursor(20, 55);
        tft.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
        tft.print("Water_Val:");
        tft.print(receive_update.water_value);
      } 
      else if (receive_update.type == UPDATE_PH) {
        tft.fillRect(120, 80, 100, 20, ILI9341_BLACK);
        tft.setCursor(20, 80);
        tft.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
        tft.print("PH_Val:");
        tft.print(receive_update.ph_value);
      } 
      else if (receive_update.type == UPDATE_FAN) {
        tft.fillRect(70, 105, 50, 20, ILI9341_BLACK);
        tft.setCursor(70, 105);
        tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
        tft.print(quat ? "ON" : "OFF");
      } 
      else if (receive_update.type == UPDATE_OXI) {
        tft.fillRect(70, 130, 50, 20, ILI9341_BLACK);
        tft.setCursor(70, 130);
        tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
        tft.print(oxi ? "ON" : "OFF");
      }
      else if (receive_update.type == UPDATE_LED3) {
        tft.fillRect(82, 155, 50, 20, ILI9341_BLACK);
        tft.setCursor(82, 155);
        tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
        tft.print(led_state3 ? "ON" : "OFF");
      }
      else if (receive_update.type == UPDATE_TIME) {
        tft.fillRect(80, 200, 150, 20, ILI9341_BLACK);
        tft.setCursor(80, 200);
        tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
        tft.print(receive_update.time_str);
      }
      vTaskDelay(100 / portTICK_PERIOD_MS); 
    }
  }
}

void adjust_time(void *pvParameters){
  bool last_adjust_state = HIGH;
  bool adjusting = false; 
  while(true){
    bool current_adjust_state = digitalRead(CLEAR_DS);
    if(current_adjust_state == LOW && last_adjust_state == HIGH){
      th_adjust = !th_adjust; // Đảo trạng thái điều chỉnh
      Serial.println(th_adjust ? "CHINH_TG" : "HET_CHINH_TG");
      
      if (th_adjust) {
        // Vào chế độ chỉnh thời gian
        tft.fillScreen(ILI9341_BLACK);
        tft.setCursor(20, 50);
        tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
        tft.setTextSize(3);
        tft.println("Adjust Time:");

        tft.setCursor(20, 100);
        tft.print("Hour: ");
        tft.print(*hour_ptr);

        tft.setCursor(20, 150);
        tft.print("Minute: ");
        tft.print(*min_ptr);

        // Suspend các task khác
        vTaskSuspend(received_display_data);
        vTaskSuspend(display_ili);
        adjusting = true;
      } else {
        // Thoát khỏi chế độ chỉnh thời gian
        tft.fillScreen(ILI9341_BLACK);
        ve_lai(); 

        vTaskResume(received_display_data);
        vTaskResume(display_ili);

        adjusting = false;
      }
    }

    // Kiểm tra nếu đang điều chỉnh giờ/phút
    if (adjusting) {
      if (digitalRead(BUTTON) == LOW) {
        // Tăng giờ khi nhấn nút BUTTON
        (*hour_ptr)++;
        if (*hour_ptr >= 24) {
          *hour_ptr = 0;
        }

        // Cập nhật giá trị giờ lên LCD
        tft.fillRect(120, 100, 100, 30, ILI9341_BLACK);
        tft.setCursor(120, 100);
        tft.print(*hour_ptr);
        delay(200); // Đợi để tránh tăng quá nhanh khi giữ nút
      }

      if (digitalRead(BUTTON2) == LOW) {
        // Tăng phút khi nhấn nút BUTTON2
        (*min_ptr)++;
        if (*min_ptr >= 60) {
          *min_ptr = 0;
        }

        // Cập nhật giá trị phút lên LCD
        tft.fillRect(120, 150, 100, 30, ILI9341_BLACK);
        tft.setCursor(120, 150);
        tft.print(*min_ptr);
        delay(200); // Đợi để tránh tăng quá nhanh khi giữ nút
      }
    }

    last_adjust_state = current_adjust_state;
    vTaskDelay(10 / portTICK_PERIOD_MS); // Đợi một chút để tránh bounce nút
  }
}

void ve_lai() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_BLUE, ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 10);
  tft.println("DO AN CHUYEN NGANH");

  tft.setCursor(20, 105);
  tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
  tft.print("FAN: ");
  tft.setCursor(70, 105);
  tft.print(quat ? "ON" : "OFF");

  tft.setCursor(20, 130);
  tft.print("OXI: ");
  tft.setCursor(70, 130);
  tft.print(oxi ? "ON" : "OFF");

  tft.setCursor(20, 155);
  tft.print("LED3: ");
  tft.setCursor(82, 155);
  tft.print(led_state3 ? "ON" : "OFF");

  tft.setCursor(20, 200);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.print("Time: ");
}


void push_button(void *pvParameters) {
  bool last_button_state = HIGH;
  while (true) {
    bool current_button_state = digitalRead(BUTTON);
    if (current_button_state == LOW && last_button_state == HIGH) {
      quat = !quat;
      Serial.println(quat ? "FAN_ON" : "FAN_OFF");
      zigbeeSerial.println(quat ? "FAN_ON" : "FAN_OFF");

      DisplayUpdate update = {UPDATE_FAN};
      xQueueSend(displayQueue, &update, portMAX_DELAY);
    }
    last_button_state = current_button_state;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void push_button2(void *pvParameters) {
  bool last_button_state2 = HIGH;
  while (true) {
    bool current_button_state2 = digitalRead(BUTTON2);
    if (current_button_state2 == LOW && last_button_state2 == HIGH) {
      oxi = !oxi;
      Serial.println(oxi ? "OXI_ON" : "OXI_OFF");
      zigbeeSerial.println(oxi ? "OXI_ON" : "OXI_OFF");

      DisplayUpdate update = { UPDATE_OXI };
      xQueueSend(displayQueue, &update, portMAX_DELAY);
    }
    last_button_state2 = current_button_state2;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void send_timer(void *pvParameters) {
  //Struct mặc định của ESP32 khi muốn dùng RTC tích hợp
  struct tm timeinfo;
  while (true) {
    //Tham số thứ 1 là 1 struct timeinfo mới tạo trên kia, tham số thứ 2 là thời gian chờ tối đa để lấy data từ sever
    //Quá thời gian này, hàm này trả về false
    if (getLocalTime(&timeinfo, 7000)) {
      //Sử dụng giờ và giấc mặc định có trong struct của ESP32 RTC
      int hour = timeinfo.tm_hour;
      int min = timeinfo.tm_min;
      //Nếu đúng giờ và đúng phút
      if ((hour == 21 && min == 36) || (hour == 6 && min == 0)) {
        //Đảo trạng thái led 3
        led_state3 = true;
        Serial.println("LED3_ON");
        //Gửi tín hiệu zigbee để bật led 3
        zigbeeSerial.println("LED3_ON");
        //Ngay lập tức gửi task cập nhật lcd vào 
        DisplayUpdate update = {UPDATE_LED3};
        xQueueSend(displayQueue, &update, portMAX_DELAY);

        //Tạo 1 biến bool để kiểm tra tín hiệu trả về từ bên "KHỐI CHẤP HÀNH"
        //Sau 2s, nếu xác nhận có tín hiệu trả về thì ok, break luôn
        bool ack_received = false;
        unsigned long start_time = millis();
        while (millis() - start_time < 2000) {  
          if (zigbeeSerial.available() > 0) {
            //Đọc chuỗi đến kí tự xuống hàng
            String ack = zigbeeSerial.readStringUntil('\n');
            ack.trim();
            if (ack == "ACK_LED3_ON") {
              ack_received = true;
              break;
            }
          }
        }

        //Nếu không nhận được cái gì trả về, thì không xác định được bên kia có bật hay chưa, và sẽ thử lại
        if (!ack_received) {
          Serial.println("No ACK received for LED3_ON, retrying...");
        }
        //Duy trì 5s
        vTaskDelay(5000 / portTICK_PERIOD_MS);

        //Sau 5s, đảo trạng thái led 1 lần nữa, tắt led 3 bên kia
        led_state3 = false;
        Serial.println("LED3_OFF");
        zigbeeSerial.println("LED3_OFF");
        //1 lần nữa, gửi task update lcd vào hàng đợi
        update = {UPDATE_LED3};
        xQueueSend(displayQueue, &update, portMAX_DELAY);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
      }
    }
    //Delay task này trong 1s chứ không nên quá lâu để xui xẻo miss mất thời gian đã thiết đặt
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void receiveDisplayDataTask(void *pvParameters) {
  while (true) {
    if (zigbeeSerial.available() > 0) {
      String data = zigbeeSerial.readStringUntil('\n');
      data.trim();

      int tdsIndex = data.indexOf("TDS_Value:");
      int WaterIndex = data.indexOf("Water_Level:");
      int phIndex = data.indexOf("PH_Value:");
      
      if (tdsIndex != -1) {
        String tdsStr = data.substring(tdsIndex + 10);
        tdsStr.trim();
        float tds_value = tdsStr.toFloat();
        DisplayUpdate send_update = {UPDATE_TDS, tds_value};
        xQueueSend(displayQueue, &send_update, portMAX_DELAY);
      }
      if (WaterIndex != -1) {
        String waterStr = data.substring(WaterIndex + 12);
        waterStr.trim();
        float water_value = waterStr.toFloat(); 
        DisplayUpdate send_update = {UPDATE_WATER, 0.0, water_value};
        xQueueSend(displayQueue, &send_update, portMAX_DELAY);
      }
      if(phIndex != -1){
        String phStr = data.substring(phIndex +9);
        phStr.trim();
        float ph_value = phStr.toFloat();
        DisplayUpdate send_update = {UPDATE_PH, 0.0, 0.0, ph_value};
        xQueueSend(displayQueue, &send_update, portMAX_DELAY);
      }
    }
    vTaskDelay(1500 / portTICK_PERIOD_MS);
  }
}

void loop() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    //Tạo 1 chuỗi để lưu toàn bộ giá trị thời gian
    char time_str_buff[40];
    //Hàm có sẵn trong thư viện time.h, nó giúp chuyển toàn bộ giá trị RTC thành 1 chuỗi char
    //Cú pháp: size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
    strftime(time_str_buff, sizeof(time_str_buff), "%H:%M  %d-%m", &timeinfo);
    //Tạo 1 chuỗi "giờ hiện tại" và lấy từ chuỗi "time_str_buff" gán vào
    String current_time = String(time_str_buff);
    //Lại tiếp tục dục nó vào hàng đợi cập nhật LCD
    //Lưu ý về 2 số 0.0 
    DisplayUpdate update = {UPDATE_TIME, 0.0, 0.0, 0.0, current_time};
    xQueueSend(displayQueue, &update, portMAX_DELAY);
  } else {
    Serial.println("Khong biet gio giac");
  }
  //Mỗi 10s cập nhật giờ 1 lần 
  vTaskDelay(10000 / portTICK_PERIOD_MS);
}

