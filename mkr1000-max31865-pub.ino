
#include <SPI.h>
#include <WiFi101.h>
#include <WiFiUdp.h>
#include <RTCZero.h>
#include "config.h"

// The <Arduino.h> header defines max and min macros.
// It really shouldn't do that.
#undef max
#undef min

#include <limits>

int status = WL_IDLE_STATUS;
// if you don't want to use DNS (and reduce your sketch size)
// use the numeric IP instead of the name for the server:
//IPAddress server(74,125,232,128);  // numeric IP for Google (no DNS)
char server[] = "192.168.2.33"; // name address for Google (using DNS)

static constexpr auto port = 8080;

RTCZero rtc;

// Initialize the Ethernet client library
// with the IP address and port of the server
// that you want to connect to (port 80 is default for HTTP):
WiFiClient client;

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

static void request()
{
    Serial.println("\nStarting connection to server...");

    // if you get a connection, report back via serial:
    if( client.connect(server, port) )
    {
        Serial.println("connected to server");
        // Make a HTTP request:
        client.println("GET /search?q=arduino HTTP/1.1");
        client.println("Host: www.google.com");
        client.println("Connection: close");
        client.println();

        last_request = time::now();

        Serial.print("now = ");
        Serial.println(rtc.getEpoch());

        state = response;
    }
}

static void response()
{
    // if there are incoming bytes available
    // from the server, read them and print them:
    while( client.available() )
    {
        char c = client.read();
        Serial.write(c);
    }

    // if the server's disconnected, stop the client:
    if( ! client.connected() )
    {
        Serial.println();
        Serial.println("disconnecting from server.");
        client.stop();

        state = wait;
    }
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

    while( ! Serial )
    {
        // wait for serial port to connect. Needed for native USB port only
    }

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

    while( status == WL_CONNECTED )
    {
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
