/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#ifdef USE_TIMER

#include "build/atomic.h"

#include "common/utils.h"

#include "drivers/nvic.h"

#include "drivers/io.h"
#include "rcc.h"
#include "drivers/system.h"

#include "timer.h"
#include "timer_impl.h"

#define TIM_N(n) (1 << (n))

/*
    Groups that allow running different period (ex 50Hz servos + 400Hz throttle + etc):
    TIM1 2 channels
    TIM2 4 channels
    TIM3 4 channels
    TIM4 4 channels
*/

#define USED_TIMER_COUNT BITCOUNT(USED_TIMERS)
#define CC_CHANNELS_PER_TIMER 4              // TIM_Channel_1..4

/*
 * 原来channel 定义是 TIM_Channel_1、2、3、4 = 0、4、8、C  所以 channel /4= 0,1,2,3
 * 位移是正确的，但是自己这里channel 改成了 1、2、3、4 ，所以改成 ch-1
 * 在具体的input channel 中 at32的tmr_channel_select_type 是0 2 4 6 需要映射为 (channel-1)*2
 */
#define TIM_IT_CCx(ch) (TMR_C1_INT << ((ch)-1))

#define TIM_CH_TO_SELCHANNEL(ch)  (( ch -1)*2)

typedef struct timerConfig_s {
    // per-timer
    timerOvrHandlerRec_t *updateCallback;

    // per-channel
    timerCCHandlerRec_t *edgeCallback[CC_CHANNELS_PER_TIMER];
    timerOvrHandlerRec_t *overflowCallback[CC_CHANNELS_PER_TIMER];

    // state
    timerOvrHandlerRec_t *overflowCallbackActive; // null-terminated linked list of active overflow callbacks
    uint32_t forcedOverflowTimerValue;
} timerConfig_t;
timerConfig_t timerConfig[USED_TIMER_COUNT];

typedef struct {
    channelType_t type;
} timerChannelInfo_t;

timerChannelInfo_t timerChannelInfo[TIMER_CHANNEL_COUNT];

typedef struct {
    uint8_t priority;
} timerInfo_t;
timerInfo_t timerInfo[USED_TIMER_COUNT];

// return index of timer in timer table. Lowest timer has index 0
#define TIMER_INDEX(i) BITCOUNT((TIM_N(i) - 1) & USED_TIMERS)

static uint8_t lookupTimerIndex(const tmr_type *tim)
{
#define _CASE_SHF 10           // amount we can safely shift timer address to the right. gcc will throw error if some timers overlap
#define _CASE_(tim, index) case ((unsigned)tim >> _CASE_SHF): return index; break
#define _CASE(i) _CASE_(TMR##i##_BASE, TIMER_INDEX(i))

// let gcc do the work, switch should be quite optimized
    switch ((unsigned)tim >> _CASE_SHF) {
#if USED_TIMERS & TIM_N(1)
        _CASE(1);
#endif
#if USED_TIMERS & TIM_N(2)
        _CASE(2);
#endif
#if USED_TIMERS & TIM_N(3)
        _CASE(3);
#endif
#if USED_TIMERS & TIM_N(4)
        _CASE(4);
#endif
#if USED_TIMERS & TIM_N(5)
        _CASE(5);
#endif
#if USED_TIMERS & TIM_N(6)
        _CASE(6);
#endif
#if USED_TIMERS & TIM_N(7)
        _CASE(7);
#endif
#if USED_TIMERS & TIM_N(8)
        _CASE(8);
#endif
#if USED_TIMERS & TIM_N(9)
        _CASE(9);
#endif
#if USED_TIMERS & TIM_N(10)
        _CASE(10);
#endif
#if USED_TIMERS & TIM_N(11)
        _CASE(11);
#endif
#if USED_TIMERS & TIM_N(12)
        _CASE(12);
#endif
#if USED_TIMERS & TIM_N(13)
        _CASE(13);
#endif
#if USED_TIMERS & TIM_N(14)
        _CASE(14);
#endif
#if USED_TIMERS & TIM_N(15)
        _CASE(15);
#endif
#if USED_TIMERS & TIM_N(16)
        _CASE(16);
#endif
#if USED_TIMERS & TIM_N(17)
        _CASE(17);
#endif
    default:  return ~1;  // make sure final index is out of range
    }
#undef _CASE
#undef _CASE_
}

tmr_type * const usedTimers[USED_TIMER_COUNT] = {
#define _DEF(i) TMR##i

#if USED_TIMERS & TIM_N(1)
    _DEF(1),
#endif
#if USED_TIMERS & TIM_N(2)
    _DEF(2),
#endif
#if USED_TIMERS & TIM_N(3)
    _DEF(3),
#endif
#if USED_TIMERS & TIM_N(4)
    _DEF(4),
#endif
#if USED_TIMERS & TIM_N(5)
    _DEF(5),
#endif
#if USED_TIMERS & TIM_N(6)
    _DEF(6),
#endif
#if USED_TIMERS & TIM_N(7)
    _DEF(7),
#endif
#if USED_TIMERS & TIM_N(8)
    _DEF(8),
#endif
#if USED_TIMERS & TIM_N(9)
    _DEF(9),
#endif
#if USED_TIMERS & TIM_N(10)
    _DEF(10),
#endif
#if USED_TIMERS & TIM_N(11)
    _DEF(11),
#endif
#if USED_TIMERS & TIM_N(12)
    _DEF(12),
#endif
#if USED_TIMERS & TIM_N(13)
    _DEF(13),
#endif
#if USED_TIMERS & TIM_N(14)
    _DEF(14),
#endif
#if USED_TIMERS & TIM_N(15)
    _DEF(15),
#endif
#if USED_TIMERS & TIM_N(16)
    _DEF(16),
#endif
#if USED_TIMERS & TIM_N(17)
    _DEF(17),
#endif
#if USED_TIMERS & TIM_N(20)
    _DEF(20),
#endif
#undef _DEF
};

// Map timer index to timer number (Straight copy of usedTimers array)
const int8_t timerNumbers[USED_TIMER_COUNT] = {
#define _DEF(i) i

#if USED_TIMERS & TIM_N(1)
    _DEF(1),
#endif
#if USED_TIMERS & TIM_N(2)
    _DEF(2),
#endif
#if USED_TIMERS & TIM_N(3)
    _DEF(3),
#endif
#if USED_TIMERS & TIM_N(4)
    _DEF(4),
#endif
#if USED_TIMERS & TIM_N(5)
    _DEF(5),
#endif
#if USED_TIMERS & TIM_N(6)
    _DEF(6),
#endif
#if USED_TIMERS & TIM_N(7)
    _DEF(7),
#endif
#if USED_TIMERS & TIM_N(8)
    _DEF(8),
#endif
#if USED_TIMERS & TIM_N(9)
    _DEF(9),
#endif
#if USED_TIMERS & TIM_N(10)
    _DEF(10),
#endif
#if USED_TIMERS & TIM_N(11)
    _DEF(11),
#endif
#if USED_TIMERS & TIM_N(12)
    _DEF(12),
#endif
#if USED_TIMERS & TIM_N(13)
    _DEF(13),
#endif
#if USED_TIMERS & TIM_N(14)
    _DEF(14),
#endif
#if USED_TIMERS & TIM_N(15)
    _DEF(15),
#endif
#if USED_TIMERS & TIM_N(16)
    _DEF(16),
#endif
#if USED_TIMERS & TIM_N(17)
    _DEF(17),
#endif
#if USED_TIMERS & TIM_N(20)
    _DEF(20),
#endif
#undef _DEF
};

int8_t timerGetNumberByIndex(uint8_t index)
{
    if (index < USED_TIMER_COUNT) {
        return timerNumbers[index];
    } else {
        return 0;
    }
}

int8_t timerGetTIMNumber(const tmr_type *tim)
{
    const uint8_t index = lookupTimerIndex(tim);

    return timerGetNumberByIndex(index);
}

static inline uint8_t lookupChannelIndex(const uint16_t channel)
{
    return channel >> 2;
}

uint8_t timerLookupChannelIndex(const uint16_t channel)
{
    return lookupChannelIndex(channel);
}

rccPeriphTag_t timerRCC(tmr_type *tim)
{
    for (int i = 0; i < HARDWARE_TIMER_DEFINITION_COUNT; i++) {
        if (timerDefinitions[i].TIMx == tim) {
            return timerDefinitions[i].rcc;
        }
    }
    return 0;
}

uint8_t timerInputIrq(tmr_type *tim)
{
    for (int i = 0; i < HARDWARE_TIMER_DEFINITION_COUNT; i++) {
        if (timerDefinitions[i].TIMx == tim) {
            return timerDefinitions[i].inputIrq;
        }
    }
    return 0;
}

void timerNVICConfigure(uint8_t irq)
{
    nvic_irq_enable(irq,NVIC_PRIORITY_BASE(NVIC_PRIO_TIMER),NVIC_PRIORITY_SUB(NVIC_PRIO_TIMER));
}

void configTimeBase(tmr_type *tim, uint16_t period, uint32_t hz)
{
	//timer, period, perscaler
    tmr_base_init(tim,(period - 1) & 0xFFFF,(timerClock(tim) / hz) - 1);
    //TMR_CLOCK_DIV1 = 0X00 NO DIV
    tmr_clock_source_div_set(tim,TMR_CLOCK_DIV1);
    //COUNT UP
    tmr_cnt_dir_set(tim,TMR_COUNT_UP);

}

// old interface for PWM inputs. It should be replaced
void timerConfigure(const timerHardware_t *timerHardwarePtr, uint16_t period, uint32_t hz)
{
    configTimeBase(timerHardwarePtr->tim, period, hz);
    tmr_counter_enable(timerHardwarePtr->tim, TRUE);

    uint8_t irq = timerInputIrq(timerHardwarePtr->tim);
    timerNVICConfigure(irq);
    // HACK - enable second IRQ on timers that need it
    // FIXME: 无脑抄，可能是错的，需要回头核实是否需要开另外一个中断
    switch (irq) {
#if defined(AT32F43x)
    case TMR1_CH_IRQn:
        timerNVICConfigure(TMR1_OVF_TMR10_IRQn);
        break;
#endif
    }
}

// allocate and configure timer channel. Timer priority is set to highest priority of its channels
void timerChInit(const timerHardware_t *timHw, channelType_t type, int irqPriority, uint8_t irq)
{
    unsigned channel = timHw - TIMER_HARDWARE;
    if (channel >= TIMER_CHANNEL_COUNT) {
        return;
    }

    timerChannelInfo[channel].type = type;
    unsigned timer = lookupTimerIndex(timHw->tim);
    if (timer >= USED_TIMER_COUNT)
        return;
    if (irqPriority < timerInfo[timer].priority) {
        // it would be better to set priority in the end, but current startup sequence is not ready
        configTimeBase(usedTimers[timer], 0, 1);
        tmr_counter_enable(usedTimers[timer],  TRUE);

        nvic_irq_enable(irq,NVIC_PRIORITY_BASE(irqPriority),NVIC_PRIORITY_SUB(irqPriority));

        timerInfo[timer].priority = irqPriority;
    }
}

void timerChCCHandlerInit(timerCCHandlerRec_t *self, timerCCHandlerCallback *fn)
{
    self->fn = fn;
}

void timerChOvrHandlerInit(timerOvrHandlerRec_t *self, timerOvrHandlerCallback *fn)
{
    self->fn = fn;
    self->next = NULL;
}

// update overflow callback list
// some synchronization mechanism is neccesary to avoid disturbing other channels (BASEPRI used now)
static void timerChConfig_UpdateOverflow(timerConfig_t *cfg, const tmr_type *tim) {
    timerOvrHandlerRec_t **chain = &cfg->overflowCallbackActive;
    ATOMIC_BLOCK(NVIC_PRIO_TIMER) {

        if (cfg->updateCallback) {
            *chain = cfg->updateCallback;
            chain = &cfg->updateCallback->next;
        }

        for (int i = 0; i < CC_CHANNELS_PER_TIMER; i++)
            if (cfg->overflowCallback[i]) {
                *chain = cfg->overflowCallback[i];
                chain = &cfg->overflowCallback[i]->next;
            }
        *chain = NULL;
    }
    // enable or disable IRQ
    tmr_interrupt_enable((tmr_type *)tim, TMR_OVF_INT, cfg->overflowCallbackActive ? ENABLE : DISABLE);
}
// config edge and overflow callback for channel. Try to avoid per-channel overflowCallback, it is a bit expensive
void timerChConfigCallbacks(const timerHardware_t *timHw, timerCCHandlerRec_t *edgeCallback, timerOvrHandlerRec_t *overflowCallback)
{
    uint8_t timerIndex = lookupTimerIndex(timHw->tim);
    if (timerIndex >= USED_TIMER_COUNT) {
        return;
    }
    uint8_t channelIndex = lookupChannelIndex(timHw->channel);
    if (edgeCallback == NULL)   // disable irq before changing callback to NULL
    	tmr_interrupt_enable(timHw->tim, TIM_IT_CCx(timHw->channel), DISABLE);
    // setup callback info
    timerConfig[timerIndex].edgeCallback[channelIndex] = edgeCallback;
    timerConfig[timerIndex].overflowCallback[channelIndex] = overflowCallback;
    // enable channel IRQ
    if (edgeCallback)
    	tmr_interrupt_enable(timHw->tim, TIM_IT_CCx(timHw->channel), ENABLE);

    timerChConfig_UpdateOverflow(&timerConfig[timerIndex], timHw->tim);
}

void timerConfigUpdateCallback(const tmr_type *tim, timerOvrHandlerRec_t *updateCallback)
{
    uint8_t timerIndex = lookupTimerIndex(tim);
    if (timerIndex >= USED_TIMER_COUNT) {
        return;
    }
    timerConfig[timerIndex].updateCallback = updateCallback;
    timerChConfig_UpdateOverflow(&timerConfig[timerIndex], tim);
}
//FIXME: 20220331 HERE!
//  双定时器的用法应该用不上
// configure callbacks for pair of channels (1+2 or 3+4).
// Hi(2,4) and Lo(1,3) callbacks are specified, it is not important which timHw channel is used.
// This is intended for dual capture mode (each channel handles one transition)
//void timerChConfigCallbacksDual(const timerHardware_t *timHw, timerCCHandlerRec_t *edgeCallbackLo, timerCCHandlerRec_t *edgeCallbackHi, timerOvrHandlerRec_t *overflowCallback)
//{
//    uint8_t timerIndex = lookupTimerIndex(timHw->tim);
//    if (timerIndex >= USED_TIMER_COUNT) {
//        return;
//    }
//    uint16_t chLo = timHw->channel & ~TIM_Channel_2;   // lower channel
//    uint16_t chHi = timHw->channel | TIM_Channel_2;    // upper channel
//    uint8_t channelIndex = lookupChannelIndex(chLo);   // get index of lower channel
//
//    if (edgeCallbackLo == NULL)   // disable irq before changing setting callback to NULL
//        TIM_ITConfig(timHw->tim, TIM_IT_CCx(chLo), DISABLE);
//    if (edgeCallbackHi == NULL)   // disable irq before changing setting callback to NULL
//        TIM_ITConfig(timHw->tim, TIM_IT_CCx(chHi), DISABLE);
//
//    // setup callback info
//    timerConfig[timerIndex].edgeCallback[channelIndex] = edgeCallbackLo;
//    timerConfig[timerIndex].edgeCallback[channelIndex + 1] = edgeCallbackHi;
//    timerConfig[timerIndex].overflowCallback[channelIndex] = overflowCallback;
//    timerConfig[timerIndex].overflowCallback[channelIndex + 1] = NULL;
//
//    // enable channel IRQs
//    if (edgeCallbackLo) {
//        TIM_ClearFlag(timHw->tim, TIM_IT_CCx(chLo));
//        TIM_ITConfig(timHw->tim, TIM_IT_CCx(chLo), ENABLE);
//    }
//    if (edgeCallbackHi) {
//        TIM_ClearFlag(timHw->tim, TIM_IT_CCx(chHi));
//        TIM_ITConfig(timHw->tim, TIM_IT_CCx(chHi), ENABLE);
//    }
//
//    timerChConfig_UpdateOverflow(&timerConfig[timerIndex], timHw->tim);
//}
//
//// enable/disable IRQ for low channel in dual configuration
//void timerChITConfigDualLo(const timerHardware_t *timHw, FunctionalState newState) {
//    TIM_ITConfig(timHw->tim, TIM_IT_CCx(timHw->channel&~TIM_Channel_2), newState);
//}

// enable or disable IRQ
void timerChITConfig(const timerHardware_t *timHw, FunctionalState newState)
{
	tmr_interrupt_enable(timHw->tim, TIM_IT_CCx(timHw->channel), newState);
}

// clear Compare/Capture flag for channel
void timerChClearCCFlag(const timerHardware_t *timHw)
{
    tmr_flag_clear(timHw->tim, TIM_IT_CCx(timHw->channel));
}

// configure timer channel GPIO mode
void timerChConfigGPIO(const timerHardware_t* timHw, ioConfig_t mode)
{
    IOInit(IOGetByTag(timHw->tag), OWNER_TIMER, 0);
    IOConfigGPIO(IOGetByTag(timHw->tag), mode);
}

// calculate input filter constant
// TODO - we should probably setup DTS to higher value to allow reasonable input filtering
//   - notice that prescaler[0] does use DTS for sampling - the sequence won't be monotonous anymore
static unsigned getFilter(unsigned ticks)
{
    static const unsigned ftab[16] = {
        1*1,                 // fDTS !
        1*2, 1*4, 1*8,       // fCK_INT
        2*6, 2*8,            // fDTS/2
        4*6, 4*8,
        8*6, 8*8,
        16*5, 16*6, 16*8,
        32*5, 32*6, 32*8
    };
    for (unsigned i = 1; i < ARRAYLEN(ftab); i++)
        if (ftab[i] > ticks)
            return i - 1;
    return 0x0f;
}

// Configure input capture
void timerChConfigIC(const timerHardware_t *timHw, bool polarityRising, unsigned inputFilterTicks)
{

    tmr_input_config_type tmr_icInitStructure;
    tmr_icInitStructure.input_channel_select = TIM_CH_TO_SELCHANNEL(timHw->channel) ;// MAPS 1234 TO 0 2 4 6
    tmr_icInitStructure.input_polarity_select = polarityRising ?TMR_INPUT_RISING_EDGE:TMR_INPUT_FALLING_EDGE;
    tmr_icInitStructure.input_mapped_select = TMR_CC_CHANNEL_MAPPED_DIRECT;
    tmr_icInitStructure.input_filter_value = getFilter(inputFilterTicks);

    tmr_input_channel_init(timHw->tim,&tmr_icInitStructure,TMR_CHANNEL_INPUT_DIV_1);

}


/* 获取输入通道极性 暂时没用*/
//void timerChICPolarity(const timerHardware_t *timHw, bool polarityRising)
//{
//    timCCER_t tmpccer = timHw->tim->CCER;
//    tmpccer &= ~(TIM_CCER_CC1P << timHw->channel);
//    tmpccer |= polarityRising ? (TIM_ICPolarity_Rising << timHw->channel) : (TIM_ICPolarity_Falling << timHw->channel);
//    timHw->tim->CCER = tmpccer;
//}



volatile timCCR_t* timerChCCR(const timerHardware_t *timHw)
{

	if(timHw->channel ==1)
		return (volatile timCCR_t*)(&timHw->tim->c1dt);
	else if(timHw->channel ==2)
		return (volatile timCCR_t*)(&timHw->tim->c2dt);
	else if(timHw->channel ==3)
		return (volatile timCCR_t*)(&timHw->tim->c3dt);
	else if(timHw->channel ==4)
		return (volatile timCCR_t*)(&timHw->tim->c4dt);
	else
		return (volatile timCCR_t*)((volatile char*)&timHw->tim->c1dt + (timHw->channel-1)*0x04); //for 32bit need to debug

}

//void timerChConfigOC(const timerHardware_t* timHw, bool outEnable, bool stateHigh)
//{
//    TIM_OCInitTypeDef  TIM_OCInitStructure;
//
//    TIM_OCStructInit(&TIM_OCInitStructure);
//    if (outEnable) {
//        TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_Inactive;
//        TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
//        if (timHw->output & TIMER_OUTPUT_INVERTED) {
//            stateHigh = !stateHigh;
//        }
//        TIM_OCInitStructure.TIM_OCPolarity = stateHigh ? TIM_OCPolarity_High : TIM_OCPolarity_Low;
//    } else {
//        TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_Timing;
//    }
//
//    switch (timHw->channel) {
//    case TIM_Channel_1:
//        TIM_OC1Init(timHw->tim, &TIM_OCInitStructure);
//        TIM_OC1PreloadConfig(timHw->tim, TIM_OCPreload_Disable);
//        break;
//    case TIM_Channel_2:
//        TIM_OC2Init(timHw->tim, &TIM_OCInitStructure);
//        TIM_OC2PreloadConfig(timHw->tim, TIM_OCPreload_Disable);
//        break;
//    case TIM_Channel_3:
//        TIM_OC3Init(timHw->tim, &TIM_OCInitStructure);
//        TIM_OC3PreloadConfig(timHw->tim, TIM_OCPreload_Disable);
//        break;
//    case TIM_Channel_4:
//        TIM_OC4Init(timHw->tim, &TIM_OCInitStructure);
//        TIM_OC4PreloadConfig(timHw->tim, TIM_OCPreload_Disable);
//        break;
//    }
//}

static void timCCxHandler(tmr_type *tim, timerConfig_t *timerConfig)
{
    uint16_t capture;
    unsigned tim_status;
    tim_status = tim->ists & tim->iden;
#if 1
    while (tim_status) {
        // flags will be cleared by reading CCR in dual capture, make sure we call handler correctly
        // current order is highest bit first. Code should not rely on specific order (it will introduce race conditions anyway)
        unsigned bit = __builtin_clz(tim_status);
        unsigned mask = ~(0x80000000 >> bit);
        tim->ists = mask;
        tim_status &= mask;
        switch (bit) {
            case __builtin_clz(TMR_OVF_FLAG): {

                if (timerConfig->forcedOverflowTimerValue != 0) {
                    capture = timerConfig->forcedOverflowTimerValue - 1;
                    timerConfig->forcedOverflowTimerValue = 0;
                } else {
                    capture = tim->pr;
                }

                timerOvrHandlerRec_t *cb = timerConfig->overflowCallbackActive;
                while (cb) {
                    cb->fn(cb, capture);
                    cb = cb->next;
                }
                break;
            }
            case __builtin_clz(TMR_C1_FLAG):
                timerConfig->edgeCallback[0]->fn(timerConfig->edgeCallback[0], tim->c1dt);
                break;
            case __builtin_clz(TMR_C2_FLAG):
                timerConfig->edgeCallback[1]->fn(timerConfig->edgeCallback[1], tim->c2dt);
                break;
            case __builtin_clz(TMR_C3_FLAG):
                timerConfig->edgeCallback[2]->fn(timerConfig->edgeCallback[2], tim->c3dt);
                break;
            case __builtin_clz(TMR_C4_FLAG):
                timerConfig->edgeCallback[3]->fn(timerConfig->edgeCallback[3], tim->c4dt);
                break;
        }
    }
#else
    if (tim_status & (int)TIM_IT_Update) {
        tim->SR = ~TIM_IT_Update;
        capture = tim->ARR;
        timerOvrHandlerRec_t *cb = timerConfig->overflowCallbackActive;
        while (cb) {
            cb->fn(cb, capture);
            cb = cb->next;
        }
    }
    if (tim_status & (int)TIM_IT_CC1) {
        tim->SR = ~TIM_IT_CC1;
        timerConfig->edgeCallback[0]->fn(timerConfig->edgeCallback[0], tim->CCR1);
    }
    if (tim_status & (int)TIM_IT_CC2) {
        tim->SR = ~TIM_IT_CC2;
        timerConfig->edgeCallback[1]->fn(timerConfig->edgeCallback[1], tim->CCR2);
    }
    if (tim_status & (int)TIM_IT_CC3) {
        tim->SR = ~TIM_IT_CC3;
        timerConfig->edgeCallback[2]->fn(timerConfig->edgeCallback[2], tim->CCR3);
    }
    if (tim_status & (int)TIM_IT_CC4) {
        tim->SR = ~TIM_IT_CC4;
        timerConfig->edgeCallback[3]->fn(timerConfig->edgeCallback[3], tim->CCR4);
    }
#endif
}

// 处理定时器 update 请求
static inline void timUpdateHandler(tmr_type *tim, timerConfig_t *timerConfig)
{
    uint16_t capture;
    unsigned tim_status;
    tim_status = tim->ists & tim->iden; //这里需要注意一下， stm32的标志位少于at32 的标志位，直接& 需要对下标志位
    while (tim_status) {
        // flags will be cleared by reading CCR in dual capture, make sure we call handler correctly
        // currrent order is highest bit first. Code should not rely on specific order (it will introduce race conditions anyway)
        unsigned bit = __builtin_clz(tim_status);//判断发生多少事件
        unsigned mask = ~(0x80000000 >> bit); //移位取反制造掩模 形成
        tim->ists = mask; //清除中断标识
        tim_status &= mask; // 更新本地状态
        switch (bit) {
            case __builtin_clz(TMR_OVF_FLAG): { // tim_it_update= 0x0001 => TMR_OVF_FLAG

                if (timerConfig->forcedOverflowTimerValue != 0) {
                    capture = timerConfig->forcedOverflowTimerValue - 1;
                    timerConfig->forcedOverflowTimerValue = 0;
                } else {
                    capture = tim->pr;
                }

                timerOvrHandlerRec_t *cb = timerConfig->overflowCallbackActive;
                while (cb) {
                    cb->fn(cb, capture);
                    cb = cb->next;
                }
                break;
            }
        }
    }
}
/*comment about ists & iden
* at32 与stm32 的寄存器位设置一样
*  基本上 状态与使能能1：1 对应， 做& 运算的时候，可以直接得出当前定时器的中断使能且产生中断的判断，
*  只剩下OVFDEN 更新（dma） 使能且中断挂起
*   HALLDEN TDEN 这几个中断没有对应的状态位，默认值为0 等，不会影响
*
__IO uint32_t ovfif                : 1;    __IO uint32_t ovfien               : 1;  [0]
__IO uint32_t c1if                 : 1;    __IO uint32_t c1ien                : 1;  [1]
__IO uint32_t c2if                 : 1;    __IO uint32_t c2ien                : 1;  [2]
__IO uint32_t c3if                 : 1;    __IO uint32_t c3ien                : 1;  [3]
__IO uint32_t c4if                 : 1;    __IO uint32_t c4ien                : 1;  [4]
__IO uint32_t hallif               : 1;    __IO uint32_t hallien              : 1;  [5]
__IO uint32_t trgif                : 1;    __IO uint32_t tien                 : 1;  [6]
__IO uint32_t brkif                : 1;    __IO uint32_t brkie                : 1;  [7]
__IO uint32_t reserved1            : 1;    __IO uint32_t ovfden               : 1;  [8]
__IO uint32_t c1rf                 : 1;    __IO uint32_t c1den                : 1;  [9]
__IO uint32_t c2rf                 : 1;    __IO uint32_t c2den                : 1;  [10]
__IO uint32_t c3rf                 : 1;    __IO uint32_t c3den                : 1;  [11]
__IO uint32_t c4rf                 : 1;    __IO uint32_t c4den                : 1;  [12]
__IO uint32_t reserved2            : 19;   __IO uint32_t hallde               : 1;  [13]
                                           __IO uint32_t tden                 : 1;  [14]
                                           __IO uint32_t reserved1            : 17; [31:15]
*/


// handler for shared interrupts when both timers need to check status bits
#define _TIM_IRQ_HANDLER2(name, i, j)                                   \
    void name(void)                                                     \
    {                                                                   \
        timCCxHandler(TMR ## i, &timerConfig[TIMER_INDEX(i)]);          \
        timCCxHandler(TMR ## j, &timerConfig[TIMER_INDEX(j)]);          \
    } struct dummy

#define _TIM_IRQ_HANDLER(name, i)                                       \
    void name(void)                                                     \
    {                                                                   \
        timCCxHandler(TMR ## i, &timerConfig[TIMER_INDEX(i)]);          \
    } struct dummy

#define _TIM_IRQ_HANDLER_UPDATE_ONLY(name, i)                           \
    void name(void)                                                     \
    {                                                                   \
        timUpdateHandler(TMR ## i, &timerConfig[TIMER_INDEX(i)]);       \
    } struct dummy

#if USED_TIMERS & TIM_N(1)
_TIM_IRQ_HANDLER(TMR1_CH_IRQHandler, 1);
#endif
#if USED_TIMERS & TIM_N(2)
_TIM_IRQ_HANDLER(TMR2_GLOBAL_IRQHandler, 2);
#endif
#if USED_TIMERS & TIM_N(3)
_TIM_IRQ_HANDLER(TMR3_GLOBAL_IRQHandler, 3);
#endif
#if USED_TIMERS & TIM_N(4)
_TIM_IRQ_HANDLER(TMR4_GLOBAL_IRQHandler, 4);
#endif
#if USED_TIMERS & TIM_N(5)
_TIM_IRQ_HANDLER(TMR5_GLOBAL_IRQHandler, 5);
#endif
#if USED_TIMERS & TIM_N(6)
_TIM_IRQ_HANDLER(TMR6_DAC_GLOBAL_IRQHandler, 6);
#endif

#if USED_TIMERS & TIM_N(7)
_TIM_IRQ_HANDLER(TMR7_GLOBAL_IRQHandler, 7);
#endif

#if USED_TIMERS & TIM_N(8)
_TIM_IRQ_HANDLER(TMR8_CH_IRQnHandler, 8);
#endif
#if USED_TIMERS & TIM_N(9)
_TIM_IRQ_HANDLER(TMR1_BRK_TMR9_IRQnHandler, 9);
#endif
//fixme: there may be a bug
#if USED_TIMERS & TIM_N(10)
_TIM_IRQ_HANDLER2(TMR1_OVF_TMR10_IRQnHandler, 1,10);
#endif
#  if USED_TIMERS & TIM_N(11)
_TIM_IRQ_HANDLER(TMR1_TRG_HALL_TMR11_IRQnHandler, 11);
#  endif
#if USED_TIMERS & TIM_N(12)
_TIM_IRQ_HANDLER(TMR8_BRK_TMR12_IRQnHandler, 12);
#endif
#if USED_TIMERS & TIM_N(13)
_TIM_IRQ_HANDLER(TMR8_OVF_TMR13_IRQnHandler, 13);
#endif
#if USED_TIMERS & TIM_N(14)
_TIM_IRQ_HANDLER(TMR8_TRG_HALL_TMR14_IRQnHandler, 14);
#endif
#if USED_TIMERS & TIM_N(20)
_TIM_IRQ_HANDLER(TMR20_CH_IRQnHandler, 20);
#endif


void timerInit(void)
{
    memset(timerConfig, 0, sizeof(timerConfig));

#if defined(PARTIAL_REMAP_TIM3)
    GPIO_PinRemapConfig(GPIO_PartialRemap_TIM3, ENABLE);
#endif

    /* enable the timer peripherals */
    for (unsigned i = 0; i < TIMER_CHANNEL_COUNT; i++) {
        RCC_ClockCmd(timerRCC(TIMER_HARDWARE[i].tim), ENABLE);
    }

    // initialize timer channel structures
    for (unsigned i = 0; i < TIMER_CHANNEL_COUNT; i++) {
        timerChannelInfo[i].type = TYPE_FREE;
    }

    for (unsigned i = 0; i < USED_TIMER_COUNT; i++) {
        timerInfo[i].priority = ~0;
    }
}

// finish configuring timers after allocation phase
// start timers
// TODO - Work in progress - initialization routine must be modified/verified to start correctly without timers
void timerStart(void)
{
#if 0
    for (unsigned timer = 0; timer < USED_TIMER_COUNT; timer++) {
        int priority = -1;
        int irq = -1;
        for (unsigned hwc = 0; hwc < TIMER_CHANNEL_COUNT; hwc++)
            if ((timerChannelInfo[hwc].type != TYPE_FREE) && (TIMER_HARDWARE[hwc].tim == usedTimers[timer])) {
                // TODO - move IRQ to timer info
                irq = TIMER_HARDWARE[hwc].irq;
            }
        // TODO - aggregate required timer parameters
        configTimeBase(usedTimers[timer], 0, 1);
        TIM_Cmd(usedTimers[timer],  ENABLE);
        if (priority >= 0) {  // maybe none of the channels was configured
            NVIC_InitTypeDef NVIC_InitStructure;

            NVIC_InitStructure.NVIC_IRQChannel = irq;
            NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = NVIC_SPLIT_PRIORITY_BASE(priority);
            NVIC_InitStructure.NVIC_IRQChannelSubPriority = NVIC_SPLIT_PRIORITY_SUB(priority);
            NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
            NVIC_Init(&NVIC_InitStructure);
        }
    }
#endif
}

/**
 * Force an overflow for a given timer.
 * Saves the current value of the counter in the relevant timerConfig's forcedOverflowTimerValue variable.
 * @param tmr_type *tim The timer to overflow
 * @return void
 **/
void timerForceOverflow(tmr_type *tim)
{
    uint8_t timerIndex = lookupTimerIndex((const tmr_type *)tim);

    ATOMIC_BLOCK(NVIC_PRIO_TIMER) {
        // Save the current count so that PPM reading will work on the same timer that was forced to overflow
        timerConfig[timerIndex].forcedOverflowTimerValue = tim->cval + 1;

        // Force an overflow by setting the UG bit
        tim->swevt_bit.ovfswtr =1 ; //注意检查之前是否 ovfen=1 ovf 中断使能
    }
}


/* 输出通道初始化
 * tim  timer x
 * channel  1,2,3,4 , not support N channel for now 暂时不支持互补输出通道
 * init output init config
 */
//tmr_output_channel_config
void timerOCInit(tmr_type *tim, uint8_t channel, tmr_output_config_type *init)
{

	tmr_output_channel_config(tim,TIM_CH_TO_SELCHANNEL(channel),init);
}
//tmr_output_channel_buffer_enable
void timerOCPreloadConfig(tmr_type *tim, uint8_t channel, uint16_t preload)
{
    tmr_output_channel_buffer_enable(tim,TIM_CH_TO_SELCHANNEL(channel),preload);
}

//tmr_channel_value_get
volatile timCCR_t* timerCCR(tmr_type *tim, uint8_t channel)
{

	if(channel ==1)
		return (volatile timCCR_t*)(&tim->c1dt);
	else if(channel ==2)
		return (volatile timCCR_t*)(&tim->c2dt);
	else if(channel ==3)
		return (volatile timCCR_t*)(&tim->c3dt);
	else if(channel ==4)
		return (volatile timCCR_t*)(&tim->c4dt);
	else
		return (volatile timCCR_t*)((volatile char*)&tim->c1dt + (channel-1)*0x04); //for 32bit need to debug

}

uint16_t timerDmaSource(uint8_t channel)
{
    switch (channel) {
    case 1:
        return TMR_C1_DMA_REQUEST;
    case 2:
        return TMR_C2_DMA_REQUEST;
    case 3:
        return TMR_C3_DMA_REQUEST;
    case 4:
        return TMR_C4_DMA_REQUEST;
    }
    return 0;
}

uint16_t timerGetPrescalerByDesiredMhz(tmr_type *tim, uint16_t mhz)
{
    return timerGetPrescalerByDesiredHertz(tim, MHZ_TO_HZ(mhz));
}

uint16_t timerGetPeriodByPrescaler(tmr_type *tim, uint16_t prescaler, uint32_t hz)
{
    return (uint16_t)((timerClock(tim) / (prescaler + 1)) / hz);
}

uint16_t timerGetPrescalerByDesiredHertz(tmr_type *tim, uint32_t hz)
{
    // protection here for desired hertz > SystemCoreClock???
    if (hz > timerClock(tim)) {
        return 0;
    }
    return (uint16_t)((timerClock(tim) + hz / 2 ) / hz) - 1;
}
#endif
