#pragma once

#include "MeshRadio.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include <Arduino.h>
#include <functional>

/*
  TX_LOG      - Time on air this device has transmitted

  RX_LOG      - Time on air used by valid and routable mesh packets, does not include
                TX air time

  RX_ALL_LOG  - Time of all received lora packets. This includes packets that are not
                for meshtastic devices. Does not include TX air time.

  Example analytics:

  TX_LOG + RX_LOG = Total air time for a particular meshtastic channel.

  TX_LOG + RX_LOG = Total air time for a particular meshtastic channel, including
                    other lora radios.

  RX_ALL_LOG - RX_LOG = Other lora radios on our frequency channel.
*/

#define CHANNEL_UTILIZATION_PERIODS 6
#define SECONDS_PER_PERIOD 3600
#define PERIODS_TO_LOG 8
#define MINUTES_IN_HOUR 60
#define SECONDS_IN_MINUTE 60
#define MS_IN_MINUTE (SECONDS_IN_MINUTE * 1000)
#define MS_IN_HOUR (MINUTES_IN_HOUR * SECONDS_IN_MINUTE * 1000)
#define RX_WINDOW_INTERVAL_SECONDS 600   // fw+ 10min
#define RX_WINDOW_COUNT 40               // fw+ 40 probes


enum reportTypes { TX_LOG, RX_LOG, RX_ALL_LOG };

void logAirtime(reportTypes reportType, uint32_t airtime_ms);

uint32_t *airtimeReport(reportTypes reportType);

#define ACTIVITY_WINDOW_COUNT 10
#define RXTXALL_ACTIVITY_COUNT 40
struct ActivityTime {
    uint32_t rx_time;  
    uint32_t tx_time; 
    uint32_t idle_time;
    uint32_t rx_bad_time;
};

struct RxTxAllCounter {
  uint32_t rxTxAll_counter;
};

void updateActivityWindow(const ActivityTime &newData);

class AirTime : private concurrency::OSThread
{

  public:
    AirTime();

    void logAirtime(reportTypes reportType, uint32_t airtime_ms);
    float channelUtilizationPercent();
    float utilizationTXPercent();

    float UtilizationPercentTX();
    uint32_t channelUtilization[CHANNEL_UTILIZATION_PERIODS] = {0};
    uint32_t utilizationTX[MINUTES_IN_HOUR] = {0};

    void airtimeRotatePeriod();
    uint8_t getPeriodsToLog();
    uint32_t getSecondsPerPeriod();
    uint32_t getSecondsSinceBoot();
    uint32_t *airtimeReport(reportTypes reportType);
    uint8_t getSilentMinutes(float txPercent, float dutyCycle);
    bool isTxAllowedChannelUtil(bool polite = false);
    bool isTxAllowedAirUtil();
    uint8_t max_channel_util_percent = 40;
    uint8_t polite_channel_util_percent = 25;
    uint8_t polite_duty_cycle_percent = 50; // half of Duty Cycle allowance is ok for metadata
    uint32_t rxWindowAverages[RX_WINDOW_COUNT] = {0}; //fw+
    uint32_t currentRxWindowSum = 0;    // fw+
    uint32_t currentRxWindowCount = 0;  // fw+

    ActivityTime activityWindow[ACTIVITY_WINDOW_COUNT] = {}; // fw+
    void updateActivityWindow(const ActivityTime &newData); // fw+

    RxTxAllCounter rxTxAllActivities[RXTXALL_ACTIVITY_COUNT] = {}; // fw+ 
    uint32_t rxTxAllActivitiesCount = 0; // fw+ 
    float rx_avg_60_min = 0; // fw+ 



  private:
    bool firstTime = true;
    uint8_t lastUtilPeriod = 0;
    uint8_t lastUtilPeriodTX = 0;
    uint32_t secSinceBoot = 0;
    struct airtimeStruct {
        uint32_t periodTX[PERIODS_TO_LOG];     // AirTime transmitted
        uint32_t periodRX[PERIODS_TO_LOG];     // AirTime received and repeated (Only valid mesh packets)
        uint32_t periodRX_ALL[PERIODS_TO_LOG]; // AirTime received regardless of valid mesh packet. Could include noise.
        uint8_t lastPeriodIndex;
    } airtimes;

    uint8_t getPeriodUtilMinute();
    uint8_t getPeriodUtilHour();
    uint8_t currentPeriodIndex();
    void pushNewRxWindowAverage(uint32_t average); //fw+
    uint32_t prevPeriodTX = 0; //fw+
    uint32_t prevPeriodRX = 0; //fw+
    uint32_t txAccum10 = 0; //fw+
    uint32_t rxAccum10 = 0; //fw+
    uint32_t rxBadAccum10 = 0; //fw+ 

  protected:
    virtual int32_t runOnce() override;
};

extern AirTime *airTime;
