#if !defined(ESP32)
#error Only supports ESP32
#endif

#ifndef PWMDecoder_h_
#define PWMDecoder_h_

#include "ReelTwo.h"
#include "core/SetupEvent.h"
#include "core/AnimatedEvent.h"
#include "esp32-hal.h"

// Use a separate guard macro to avoid conflicting with the rmt_ch_dir_t enum
// value RMT_RX_MODE defined in esp32-hal-rmt.h (Arduino ESP32 core 3.x)
#ifndef USE_RMT_DECODER
#define USE_RMT_DECODER 1
#endif

class PWMDecoder: public AnimatedEvent
{
public:
	PWMDecoder(void (*changeNotify)(int pin, uint16_t pwm), int pin1, int pin2 = -1, int pin3 = -1, int pin4 = -1) :
		fChangeNotify(changeNotify)
	{
		fChannel[0].fGPIO = uint8_t(pin1);
		fChannel[1].fGPIO = uint8_t(pin2);
		fChannel[2].fGPIO = uint8_t(pin3);
		fChannel[3].fGPIO = uint8_t(pin4);
		fNumChannels = 4;
		if (pin4 == -1)
			fNumChannels = 3;
		if (pin3 == -1)
			fNumChannels = 2;
		if (pin2 == -1)
			fNumChannels = 1;
	}

	PWMDecoder(int pin1, int pin2 = -1, int pin3 = -1, int pin4 = -1) :
		PWMDecoder(nullptr, pin1, pin2, pin3, pin4)
	{
	}

	inline uint16_t channel(unsigned i = 0) const
	{
		return fChannel[i].fRawPulse;
	}

	inline uint16_t getValue(unsigned i = 0) const
	{
		return fChannel[i].fRawPulse;
	}

	inline bool hasChanged(unsigned i = 0)
	{
		return abs(int32_t(fChannel[i].fPulse) - int32_t(fChannel[i].fRawPulse)) > 20;
	}

	inline unsigned numChannels() const
	{
		return fNumChannels;
	}

	unsigned long getAge(unsigned i = 0)
	{
		return millis() - fChannel[i].fLastActive;
	}

	bool isActive(unsigned i = 0)
	{
		return millis() > 500 && (fChannel[i].fPulse != 0 && getAge(i) < 500);
	}

	bool becameActive(unsigned i = 0)
	{
		return (fChannel[i].fAliveStateChange && fChannel[i].fAlive);
	}

	bool becameInactive(unsigned i = 0)
	{
		return (fChannel[i].fAliveStateChange && !fChannel[i].fAlive);
	}

	virtual void animate() override
	{
		for (unsigned i = 0; i < fNumChannels; i++)
		{
		#ifdef USE_RMT_DECODER
			if (fStarted && rmtReceiveCompleted(fChannel[i].fGPIO))
			{
				if (fChannel[i].fRMTDataLen >= 1)
				{
					fChannel[i].fRawPulse = fChannel[i].fRMTData[0].duration0;
					fChannel[i].fLastActive = millis();
				}
				fChannel[i].fRMTDataLen = 4;
				rmtReadAsync(fChannel[i].fGPIO, fChannel[i].fRMTData, &fChannel[i].fRMTDataLen);
			}
		#else
			checkActive(i);
		#endif
			fChannel[i].fAliveStateChange = false;
			if (isActive(i))
			{
				if (!fChannel[i].fAlive)
				{
					// DEBUG_PRINT("PWM Start Pin: "); DEBUG_PRINTLN(fChannel[i].fGPIO);
					fChannel[i].fAlive = true;
					fChannel[i].fAliveStateChange = true;
				}
				// if (fChangeNotify != nullptr)
				// 	fChangeNotify(fChannel[i].fGPIO, fChannel[i].fPulse);
			}
			else if (fChannel[i].fAlive)
			{
				// DEBUG_PRINT("PWM End Pin: "); DEBUG_PRINTLN(fChannel[i].fGPIO);
				fChannel[i].fAlive = false;
				fChannel[i].fAliveStateChange = true;
			}
			if (hasChanged(i))
			{
				fChannel[i].fPulse = fChannel[i].fRawPulse;
				if (fChangeNotify != nullptr)
					fChangeNotify(fChannel[i].fGPIO, fChannel[i].fPulse);
			}
		}
	}

#ifdef USE_RMT_DECODER
	bool fStarted = false;
	void begin()
	{
		if (fStarted)
			return;
		for (unsigned i = 0; i < fNumChannels; i++)
		{
		    if (rmtInit(fChannel[i].fGPIO, RMT_RX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000u))
		    {
				rmtSetRxMinThreshold(fChannel[i].fGPIO, 100);
			    rmtSetRxMaxThreshold(fChannel[i].fGPIO, 5000);
			    fChannel[i].fRMTDataLen = 4;
			    rmtReadAsync(fChannel[i].fGPIO, fChannel[i].fRMTData, &fChannel[i].fRMTDataLen);
		    }
		    else
		    {
		    	DEBUG_PRINT("FAILED TO INIT RMT on ");
		    	DEBUG_PRINTLN(fChannel[i].fGPIO);
		    }
		}
		fStarted = true;
	}

	void end()
	{
		if (!fStarted)
			return;
		for (unsigned i = 0; i < fNumChannels; i++)
		{
			rmtDeinit(fChannel[i].fGPIO);
		}
		fStarted = false;
	}
#else
	void begin();
	void end();
#endif

private:
	uint8_t fNumChannels = 1;
	static constexpr unsigned fMaxChannels = 8;
	struct Channel
	{
		uint8_t fGPIO;
		uint16_t fPulse;
		volatile uint16_t fRawPulse;
		bool fAlive;
		bool fAliveStateChange;
		uint32_t fLastActive;
	#ifdef USE_RMT_DECODER
		rmt_data_t fRMTData[4];
		size_t fRMTDataLen = 4;
	#endif
	} fChannel[fMaxChannels] = {};
	void (*fChangeNotify)(int pin, uint16_t pwm) = nullptr;

#ifdef USE_RMT_DECODER
	static void receive_data(uint32_t* data, size_t len, void* arg)
	{
		Channel* channel = (Channel*)arg;
		rmt_data_t* it = (rmt_data_t*)data;
		if (len >= 1)
		{
			channel->fRawPulse = it[0].duration0;
			channel->fLastActive = millis();
		}
	}
#else
	rmt_isr_handle_t fISRHandle = nullptr;
	static void IRAM_ATTR rmt_isr_handler(void* arg);
	void checkActive(unsigned i);
#endif
};

#define ServoDecoder PWMDecoder

#endif
