
#include <SPI.h>
#include <WiFi101.h>
#include <WiFiUdp.h>
#include <RTCZero.h>
#include <cstdio>
#include "config.h"
#include "Sensors.h"

// The <Arduino.h> header defines max and min macros.
// It really shouldn't do that.
#undef max
#undef min

#include <limits>

int status = WL_IDLE_STATUS;

// if you don't want to use DNS (and reduce your sketch size)
// use the numeric IP instead of the name for the server:

IPAddress server(192,168,4,1);  // numeric IP (no DNS)

static constexpr auto port = 8089;

RTCZero rtc;

// Initialize the Ethernet client library
// with the IP address and port of the server
// that you want to connect to (port 80 is default for HTTP):
WiFiClient client;

WiFiUDP udp;

// Sensor Shield
static Sensors sensors;

namespace time {

    using ms_t = uint32_t;

    static ms_t now()
    {
        return millis();
    }

    static constexpr ms_t elapsed(ms_t t0, ms_t t1)
    {
        return t1 < t0 ? (std::numeric_limits<ms_t>::max() - t0 + 1) + t1 : t1 - t0;
    }

    static_assert(elapsed(0, 0) == 0, "");
    static_assert(elapsed(0xFFFFFFFFu, 0) == 1, "overflow");

    static ms_t elapsed_since(ms_t t0)
    {
        return elapsed(t0, now());
    }

    static void sleep(ms_t t)
    {
        auto const t0 = now();

        while( elapsed_since(t0) < t ) {}
    }
}

static void request();
static void response();
static void wait();

using State = void (*)();

static State state = request;
static auto last_request = time::now();

struct SensorData
{
    uint8_t config;
    uint8_t status;
    uint16_t adc;
    uint16_t hft;
    uint16_t lft;
    double temp;
};

template<typename T> T bigendian(uint8_t const* bytes, unsigned offset = 0u)
{
    T value;

    for( auto i = 0; i < sizeof(T); ++i )
    {
        value <<= 8u;
        value  |= bytes[offset + i];
    }

    return value;
}

static SensorData readSensor(unsigned ch, unsigned cs)
{
    uint8_t regs[8];

    sensors.readout(cs, regs, sizeof(regs));

    uint16_t const adc = bigendian<uint16_t>(regs, 1) >> 1u;

    // adc * 0.031249727 + (-255.9977596)
    static struct {
      double a;
      double b;      
    } const coefficients[] = {
      { 0.032068,  -262.799048 },
      { 0.031228,  -255.507519 }
    };

    return {
        regs[0],
        regs[7],
        adc,
        bigendian<uint16_t>(regs, 3),
        bigendian<uint16_t>(regs, 5),
        adc * coefficients[ch].a + coefficients[ch].b
    };
}

static void request()
{
    auto const t = rtc.getEpoch();

    SensorData const data[] = {
        readSensor(0, CS_0),
        readSensor(1, CS_1)
    };

    // use influx server time

    char influx_line[256];

    auto const size = snprintf(
        influx_line,
        sizeof(influx_line),
        "mkr1000,board=" INFLUX_BOARD_TAG " adc1=%u,t1=%f,adc2=%u,t2=%f",
        data[0].adc,
        data[0].temp,
        data[1].adc,
        data[1].temp
    );

    char log_line[256];

    snprintf(
        log_line,
        sizeof(log_line),
        "%d : %X %X, %X %X %s\r\n",
        t,
        data[0].status,
        data[0].adc,
        data[1].status,
        data[1].adc,
        influx_line
    );

    Serial.print(log_line);

    if( size > 0 )
    {
        udp.beginPacket(server, port);
        udp.write(
            reinterpret_cast<uint8_t const*>(influx_line),
            size
        );
        udp.endPacket();
    }

    last_request = time::now();

    state = wait;
}

static void response()
{

}

static void wait()
{
    if( time::elapsed_since(last_request) >= 1000 )
    {
        state = request;
    }
}

void setup()
{
    //Initialize serial and wait for port to open:
    Serial.begin(9600);

#if 0
    // wait for serial port to connect. Needed for native USB port only
    while( ! Serial ) {}
#endif

    // Print RTC time

    rtc.begin();

    Serial.print("RTC: ");
    
    Serial.print(rtc.getDay());
    Serial.print(".");
    Serial.print(rtc.getMonth());
    Serial.print(".");
    Serial.print(rtc.getYear());
    Serial.print(" ");
    Serial.print(rtc.getHours());
    Serial.print(":");
    Serial.print(rtc.getMinutes());
    Serial.print(":");
    Serial.print(rtc.getSeconds());
    Serial.println("");

    // check for the presence of the shield:
    if( WiFi.status() == WL_NO_SHIELD )
    {
        Serial.println("WiFi shield not present");
       
        for(;;) {}
    }

    // open socket on arbitrary port
    // we use it to send influx lines to the db server

    if( ! udp.begin(123) )
    {
        Serial.println("failed to open UDP port");
       
        for(;;) {}
    }

    sensors.init();
}

static bool syncNTP()
{
    for( auto i = 0u; i < 20; ++i )
    {
        if( status != WL_CONNECTED )
        {
            break;
        }

        auto const ntp_epoch = WiFi.getTime();

        if( ntp_epoch )
        {
            rtc.setEpoch(ntp_epoch);

            Serial.print("time: ");
            Serial.println(ntp_epoch);

            auto const hour   = (ntp_epoch  % 86400) / 3600;
            auto const minute = (ntp_epoch  %  3600) /   60;
            auto const second = (ntp_epoch  %    60)       ;

            Serial.print(hour);
            Serial.print(":");
            Serial.print(minute);
            Serial.print(":");
            Serial.print(second);

            return true;
        }
        else
        {
            Serial.println("failed to fetch time from NTP server");

            time::sleep(500);
        }
    }

    return false;
}

static bool connectToWifi()
{
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);
    
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);

    auto const t0 = time::now();

    while( status != WL_CONNECTED )
    {
        if( time::elapsed_since(t0) > 10000 )
        {
            Serial.println("Connection attempt failed");
            return false;
        }        
    }

    Serial.println("Connected to wifi");

    printWiFiStatus();

    syncNTP();

    return true;
}

void loop()
{
    // attempt to connect to WiFi network:
    while( status != WL_CONNECTED )
    {
        if( connectToWifi() )
        {
            state = request;
        }
    }

    state();
}

void printWiFiStatus()
{
    // print the SSID of the network you're attached to:
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    // print your WiFi shield's IP address:
    IPAddress ip = WiFi.localIP();
    Serial.print("IP Address: ");
    Serial.println(ip);

    // print the received signal strength:
    long rssi = WiFi.RSSI();
    Serial.print("signal strength (RSSI):");
    Serial.print(rssi);
    Serial.println(" dBm");
}
