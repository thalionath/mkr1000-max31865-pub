
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
        return elapsed(t0, time::now());
    }

    /**
     * © Francesco Potortì 2013 - GPLv3 - Revision: 1.13
     *
     * Send an NTP packet and wait for the response, return the Unix time
     *
     * To lower the memory footprint, no buffers are allocated for sending
     * and receiving the NTP packets.  Four bytes of memory are allocated
     * for transmision, the rest is random garbage collected from the data
     * memory segment, and the received packet is read one byte at a time.
     * The Unix time is returned, that is, seconds from 1970-01-01T00:00.
     */
    unsigned long ntpUnixTime()
    {
        WiFiUDP udp;

        static int udpInited = udp.begin(123); // open socket on arbitrary port

        const char timeServer[] = "pool.ntp.org";  // NTP server

        // Only the first four bytes of an outgoing NTP packet need to be set
        // appropriately, the rest can be whatever.
        const long ntpFirstFourBytes = 0xEC0600E3; // NTP request header

        // Fail if WiFiUdp.begin() could not init a socket
        if( ! udpInited )
        {
            return 0;
        }

        // Clear received data from possible stray received packets
        udp.flush();

        // Send an NTP request
        if (! (udp.beginPacket(timeServer, 123) // 123 is the NTP port
            && udp.write((byte *)&ntpFirstFourBytes, 48) == 48
            && udp.endPacket()))
        {
            return 0; // sending request failed
        }

        // Wait for response; check every pollIntv ms up to maxPoll times

        const int pollIntv = 150;   // poll every this many ms
        const byte maxPoll = 15;    // poll up to this many times
        int pktLen;				    // received packet length

        for( byte i=0; i < maxPoll; i++ )
        {
            if( (pktLen = udp.parsePacket()) == 48 )
            {
                break;
            }

            delay(pollIntv);
        }
        
        if( pktLen != 48 )
        {
            return 0; // no correct packet received
        }

        // Read and discard the first useless bytes
        // Set useless to 32 for speed; set to 40 for accuracy.
        constexpr byte useless = 40;

        for( byte i = 0; i < useless; ++i )
        {
            udp.read();
        }

        // Read the integer part of sending time
        unsigned long time = udp.read();	// NTP time

        for( byte i = 1; i < 4; i++ )
        {
            time = time << 8 | udp.read();
        }

        // Round to the nearest second if we want accuracy
        // The fractionary part is the next byte divided by 256: if it is
        // greater than 500ms we round to the next second; we also account
        // for an assumed network delay of 50ms, and (0.5-0.05)*256=115;
        // additionally, we account for how much we delayed reading the packet
        // since its arrival, which we assume on average to be pollIntv/2.
        time += (udp.read() > 115 - pollIntv/8);

        // Discard the rest of the packet
        udp.flush();

        // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
        constexpr auto const seventyYears = 2208988800ul;

        return time - seventyYears;		// convert NTP time to Unix time
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
    Serial.println(":");

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
        auto const unixTime = time::ntpUnixTime();

        if( unixTime )
        {
            rtc.setEpoch(unixTime);

            Serial.print("time: ");
            Serial.println(unixTime);

            // subtract seventy years:
            auto const epoch = unixTime;

            auto const hour   = (epoch  % 86400) / 3600;
            auto const minute = (epoch  %  3600) /   60;
            auto const second = (epoch  %    60)       ;

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
