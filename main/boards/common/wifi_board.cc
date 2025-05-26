#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "system_info.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <cstring>
#include <string>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http.h>
#include <esp_mqtt.h>
#include <esp_udp.h>
#include <tcp_transport.h>
#include <tls_transport.h>
#include <web_socket.h>
#include <esp_log.h>

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>

static const char *TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

static void provisioning_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  static std::string _ssid, _password;
  if (event_base == WIFI_PROV_EVENT) {
    switch (event_id) {
      case WIFI_PROV_START:
        ESP_LOGI(TAG, "Provisioning started");
        break;
      case WIFI_PROV_CRED_RECV: {
                                  wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                                  auto ssid = (const char *) wifi_sta_cfg->ssid;
                                  auto password = (const char *) wifi_sta_cfg->password;
                                  ESP_LOGI(TAG, "Received Wi-Fi credentials"
                                      "\n\tSSID     : %s\n\tPassword : %s",
                                      ssid, password);
                                  _ssid = ssid;
                                  _password = password;
                                  break;
                                }
      case WIFI_PROV_CRED_FAIL: {
                                  wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                                  ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                                      "\n\tPlease reset to factory and retry provisioning",
                                      (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                                      "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                                  wifi_prov_mgr_reset_sm_state_on_failure();
                                  break;
                                }
      case WIFI_PROV_CRED_SUCCESS: {

                                     ESP_LOGI(TAG, "Provisioning successful");
                                     auto &manager = SsidManager::GetInstance();
                                     manager.AddSsid(_ssid, _password);
                                     break;
                                   }
      default:
                                   break;
    }
  }
}

void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_prov_mgr_config_t config = {
      .scheme = wifi_prov_scheme_ble,
      .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };

    ESP_ERROR_CHECK( wifi_prov_mgr_init(config) );
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &provisioning_event_handler, NULL));
    const char *service_name = "my_device";
    const char *service_key  = "password";

    wifi_prov_security_t security = WIFI_PROV_SECURITY_0;
    const char *pop = "abcd1234";

    ESP_ERROR_CHECK( wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key) );

    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, "Open APP", "", Lang::Sounds::P3_WIFICONFIG);

    // Wait for service to complete
    wifi_prov_mgr_wait();

    // Set custom ota url
    Settings settings("wifi", true);
    settings.SetString("ota_url", "http://192.168.1.100:8081/ota");

    // Finally de-initialize the manager
    wifi_prov_mgr_deinit();
}

void WifiBoard::StartNetwork() {
    // User can press BOOT button while starting to enter WiFi configuration mode
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    // If no WiFi SSID is configured, enter WiFi configuration mode
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.Start();

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }
}

Http* WifiBoard::CreateHttp() {
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    ESP_LOGI(TAG, "WebSocket URL: %s", url.c_str());
    if (url.find("wss://") == 0) {
        return new WebSocket(new TlsTransport());
    } else {
        return new WebSocket(new TcpTransport());
    }
    return nullptr;
}

Mqtt* WifiBoard::CreateMqtt() {
    return new EspMqtt();
}

Udp* WifiBoard::CreateUdp() {
    return new EspUdp();
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    if (!wifi_config_mode_) {
        board_json += "\"ssid\":\"" + wifi_station.GetSsid() + "\",";
        board_json += "\"rssi\":" + std::to_string(wifi_station.GetRssi()) + ",";
        board_json += "\"channel\":" + std::to_string(wifi_station.GetChannel()) + ",";
        board_json += "\"ip\":\"" + wifi_station.GetIpAddress() + "\",";
    }
    board_json += "\"mac\":\"" + SystemInfo::GetMacAddress() + "\"}";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}
