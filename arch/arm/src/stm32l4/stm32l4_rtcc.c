/************************************************************************************
 * arch/arm/src/stm32l4/stm32l4_rtcc.c
 *
 *   Copyright (C) 2012-2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *           dev@ziggurat29.com (adaptations to stm32l4)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ************************************************************************************/

/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <nuttx/config.h>
#include "chip.h"

#include <stdbool.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include <arch/board/board.h>

#include "up_arch.h"

#include "stm32l4_rcc.h"
#include "stm32l4_pwr.h"
#include "stm32l4_exti.h"
#include "stm32l4_rtc.h"

#ifdef CONFIG_RTC

/************************************************************************************
 * Pre-processor Definitions
 ************************************************************************************/

/* Configuration ********************************************************************/
/* This RTC implementation supports
 *  - date/time RTC hardware
 *  - extended functions Alarm A and B
 * */

#ifndef CONFIG_RTC_DATETIME
#  error "CONFIG_RTC_DATETIME must be set to use this driver"
#endif

#ifdef CONFIG_RTC_HIRES
#  error "CONFIG_RTC_HIRES must NOT be set with this driver"
#endif

#ifndef CONFIG_STM32L4_PWR
#  error "CONFIG_STM32L4_PWR must selected to use this driver"
#endif

#ifndef CONFIG_DEBUG
#  undef CONFIG_DEBUG_RTC
#endif

#if !defined(CONFIG_RTC_MAGIC)
# define CONFIG_RTC_MAGIC (0xfacefeee)
#endif

#if !defined(CONFIG_RTC_MAGIC_REG)
# define CONFIG_RTC_MAGIC_REG (0)
#endif

/* Constants ************************************************************************/

#define SYNCHRO_TIMEOUT  (0x00020000)
#define INITMODE_TIMEOUT (0x00010000)
#define RTC_MAGIC        CONFIG_RTC_MAGIC
#define RTC_MAGIC_REG    STM32L4_RTC_BKR(CONFIG_RTC_MAGIC_REG)

/* BCD conversions */

#define rtc_reg_tr_bin2bcd(tp) \
  ((rtc_bin2bcd((tp)->tm_sec)  << RTC_TR_SU_SHIFT) | \
   (rtc_bin2bcd((tp)->tm_min)  << RTC_TR_MNU_SHIFT) | \
   (rtc_bin2bcd((tp)->tm_hour) << RTC_TR_HU_SHIFT))

#define rtc_reg_alrmr_bin2bcd(tm) \
  ((rtc_bin2bcd((tm)->tm_sec)  << RTC_ALRMR_SU_SHIFT) | \
   (rtc_bin2bcd((tm)->tm_min)  << RTC_ALRMR_MNU_SHIFT) | \
   (rtc_bin2bcd((tm)->tm_hour) << RTC_ALRMR_HU_SHIFT))

/* need to ignore DATE/DOW part of alarm; rtc_reg_alrmr_bin2bcd only encodes hms */
#define RTC_ALRMR_ENABLE              (0x80000000)

/* Debug ****************************************************************************/

#ifdef CONFIG_DEBUG_RTC
#  define rtcdbg    dbg
#  define rtcvdbg   vdbg
#  define rtclldbg  lldbg
#  define rtcllvdbg llvdbg
#else
#  define rtcdbg(x...)
#  define rtcvdbg(x...)
#  define rtclldbg(x...)
#  define rtcllvdbg(x...)
#endif

/************************************************************************************
 * Private Types
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
typedef unsigned int rtc_alarmreg_t;

struct alm_cbinfo_s
{
  volatile alm_callback_t ac_cb; /* Client callback function */
  volatile FAR void *ac_arg;     /* Argument to pass with the callback function */
};
#endif

/************************************************************************************
 * Private Data
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
/* Callback to use when an EXTI is activated  */

static struct alm_cbinfo_s g_alarmcb[RTC_ALARM_LAST];
#endif

/************************************************************************************
 * Public Data
 ************************************************************************************/

/* g_rtc_enabled is set true after the RTC has successfully initialized */

volatile bool g_rtc_enabled = false;

/************************************************************************************
 * Private Function Prototypes
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
static int rtchw_check_alrawf(void);
static int rtchw_check_alrbwf(void);
static int rtchw_set_alrmar(rtc_alarmreg_t alarmreg);
static int rtchw_set_alrmbr(rtc_alarmreg_t alarmreg);
#endif

/************************************************************************************
 * Private Functions
 ************************************************************************************/

/************************************************************************************
 * Name: rtc_dumpregs
 *
 * Description:
 *    Disable RTC write protection
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ************************************************************************************/

#ifdef CONFIG_DEBUG_RTC
static void rtc_dumpregs(FAR const char *msg)
{
  rtclldbg("%s:\n", msg);
  rtclldbg("      TR: %08x\n", getreg32(STM32L4_RTC_TR));
  rtclldbg("      DR: %08x\n", getreg32(STM32L4_RTC_DR));
  rtclldbg("      CR: %08x\n", getreg32(STM32L4_RTC_CR));
  rtclldbg("     ISR: %08x\n", getreg32(STM32L4_RTC_ISR));
  rtclldbg("    PRER: %08x\n", getreg32(STM32L4_RTC_PRER));
  rtclldbg("    WUTR: %08x\n", getreg32(STM32L4_RTC_WUTR));
  
  rtclldbg("  ALRMAR: %08x\n", getreg32(STM32L4_RTC_ALRMAR));
  rtclldbg("  ALRMBR: %08x\n", getreg32(STM32L4_RTC_ALRMBR));
  rtclldbg("  SHIFTR: %08x\n", getreg32(STM32L4_RTC_SHIFTR));
  rtclldbg("    TSTR: %08x\n", getreg32(STM32L4_RTC_TSTR));
  rtclldbg("    TSDR: %08x\n", getreg32(STM32L4_RTC_TSDR));
  rtclldbg("   TSSSR: %08x\n", getreg32(STM32L4_RTC_TSSSR));
  rtclldbg("    CALR: %08x\n", getreg32(STM32L4_RTC_CALR));
  rtclldbg("  TAMPCR: %08x\n", getreg32(STM32L4_RTC_TAMPCR));
  rtclldbg("ALRMASSR: %08x\n", getreg32(STM32L4_RTC_ALRMASSR));
  rtclldbg("ALRMBSSR: %08x\n", getreg32(STM32L4_RTC_ALRMBSSR));
  rtclldbg("MAGICREG: %08x\n", getreg32(RTC_MAGIC_REG));
}
#else
#  define rtc_dumpregs(msg)
#endif

/************************************************************************************
 * Name: rtc_dumptime
 *
 * Description:
 *    Disable RTC write protection
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ************************************************************************************/

#ifdef CONFIG_DEBUG_RTC
static void rtc_dumptime(FAR const struct tm *tp, FAR const char *msg)
{
  rtclldbg("%s:\n", msg);
#if 0
  rtclldbg("  tm_sec: %08x\n", tp->tm_sec);
  rtclldbg("  tm_min: %08x\n", tp->tm_min);
  rtclldbg(" tm_hour: %08x\n", tp->tm_hour);
  rtclldbg(" tm_mday: %08x\n", tp->tm_mday);
  rtclldbg("  tm_mon: %08x\n", tp->tm_mon);
  rtclldbg(" tm_year: %08x\n", tp->tm_year);
#else
  rtclldbg("  tm: %04d-%02d-%02d %02d:%02d:%02d\n",
           tp->tm_year+1900, tp->tm_mon+1, tp->tm_mday,
           tp->tm_hour, tp->tm_min, tp->tm_sec);
#endif
}
#else
#  define rtc_dumptime(tp, msg)
#endif

/************************************************************************************
 * Name: rtc_wprunlock
 *
 * Description:
 *    Disable RTC write protection
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ************************************************************************************/

static void rtc_wprunlock(void)
{
  /* Enable write access to the backup domain. */

  (void)stm32l4_pwr_enablebkp(true);

  /* The following steps are required to unlock the write protection on all the
   * RTC registers (except for RTC_ISR[13:8], RTC_TAFCR, and RTC_BKPxR).
   *
   * 1. Write 0xCA into the RTC_WPR register.
   * 2. Write 0x53 into the RTC_WPR register.
   *
   * Writing a wrong key re-activates the write protection.
   */

  putreg32(0xca, STM32L4_RTC_WPR);
  putreg32(0x53, STM32L4_RTC_WPR);
}

/************************************************************************************
 * Name: rtc_wprlock
 *
 * Description:
 *    Enable RTC write protection
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ************************************************************************************/

static inline void rtc_wprlock(void)
{
  /* Writing any wrong key re-activates the write protection. */

  putreg32(0xff, STM32L4_RTC_WPR);

  /* Disable write access to the backup domain. */

  (void)stm32l4_pwr_enablebkp(false);
}

/************************************************************************************
 * Name: rtc_synchwait
 *
 * Description:
 *   Waits until the RTC Time and Date registers (RTC_TR and RTC_DR) are
 *   synchronized with RTC APB clock.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

static int rtc_synchwait(void)
{
  volatile uint32_t timeout;
  uint32_t regval;
  int ret;

  /* Disable the write protection for RTC registers */

  rtc_wprunlock();

  /* Clear Registers synchronization flag (RSF) */

  regval  = getreg32(STM32L4_RTC_ISR);
  regval &= ~RTC_ISR_RSF;
  putreg32(regval, STM32L4_RTC_ISR);

  /* Now wait the registers to become synchronised */

  ret = -ETIMEDOUT;
  for (timeout = 0; timeout < SYNCHRO_TIMEOUT; timeout++)
    {
      regval = getreg32(STM32L4_RTC_ISR);
      if ((regval & RTC_ISR_RSF) != 0)
        {
          /* Synchronized */

          ret = OK;
          break;
        }
    }

  /* Re-enable the write protection for RTC registers */

  rtc_wprlock();
  return ret;
}

/************************************************************************************
 * Name: rtc_enterinit
 *
 * Description:
 *   Enter RTC initialization mode.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

static int rtc_enterinit(void)
{
  volatile uint32_t timeout;
  uint32_t regval;
  int ret;

  /* Check if the Initialization mode is already set */

  regval = getreg32(STM32L4_RTC_ISR);

  ret = OK;
  if ((regval & RTC_ISR_INITF) == 0)
    {
      /* Set the Initialization mode */

      putreg32(RTC_ISR_INIT, STM32L4_RTC_ISR);

      /* Wait until the RTC is in the INIT state (or a timeout occurs) */

      ret = -ETIMEDOUT;
      for (timeout = 0; timeout < INITMODE_TIMEOUT; timeout++)
        {
          regval = getreg32(STM32L4_RTC_ISR);
          if ((regval & RTC_ISR_INITF) != 0)
            {
              ret = OK;
              break;
            }
        }
    }

  return ret;
}

/************************************************************************************
 * Name: rtc_exitinit
 *
 * Description:
 *   Exit RTC initialization mode.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

static void rtc_exitinit(void)
{
  uint32_t regval;

  regval = getreg32(STM32L4_RTC_ISR);
  regval &= ~(RTC_ISR_INIT);
  putreg32(regval, STM32L4_RTC_ISR);
}

/************************************************************************************
 * Name: rtc_bin2bcd
 *
 * Description:
 *   Converts a 2 digit binary to BCD format
 *
 * Input Parameters:
 *   value - The byte to be converted.
 *
 * Returned Value:
 *   The value in BCD representation
 *
 ************************************************************************************/

static uint32_t rtc_bin2bcd(int value)
{
  uint32_t msbcd = 0;

  while (value >= 10)
    {
      msbcd++;
      value -= 10;
    }

  return (msbcd << 4) | value;
}

/************************************************************************************
 * Name: rtc_bin2bcd
 *
 * Description:
 *   Convert from 2 digit BCD to binary.
 *
 * Input Parameters:
 *   value - The BCD value to be converted.
 *
 * Returned Value:
 *   The value in binary representation
 *
 ************************************************************************************/

static int rtc_bcd2bin(uint32_t value)
{
  uint32_t tens = (value >> 4) * 10;
  return (int)(tens + (value & 0x0f));
}

/************************************************************************************
 * Name: rtc_setup
 *
 * Description:
 *   Performs first time configuration of the RTC.  A special value written into
 *   back-up register 0 will prevent this function from being called on sub-sequent
 *   resets or power up.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

static int rtc_setup(void)
{
  uint32_t regval;
  int ret;

  /* Disable the write protection for RTC registers */

  rtc_wprunlock();

  /* Set Initialization mode */

  ret = rtc_enterinit();
  if (ret == OK)
    {
      /* Set the 24 hour format by clearing the FMT bit in the RTC
       * control register
       */

      regval = getreg32(STM32L4_RTC_CR);
      regval &= ~RTC_CR_FMT;
      putreg32(regval, STM32L4_RTC_CR);

      /* Configure RTC pre-scaler with the required values */

#ifdef CONFIG_STM32L4_RTC_HSECLOCK
      /* For a 1 MHz clock this yields 0.9999360041 Hz on the second
       * timer - which is pretty close.
       * NOTE: max HSE is 4 MHz if it is to be used with RTC
       */

      putreg32(((uint32_t)7812 << RTC_PRER_PREDIV_S_SHIFT) |
              ((uint32_t)0x7f << RTC_PRER_PREDIV_A_SHIFT),
              STM32L4_RTC_PRER);
#elif defined(CONFIG_STM32L4_RTC_LSICLOCK)
      /* Suitable values for 32.000 KHz LSI clock (29.5 - 34 KHz, though) */

      putreg32(((uint32_t)0xf9 << RTC_PRER_PREDIV_S_SHIFT) |
              ((uint32_t)0x7f << RTC_PRER_PREDIV_A_SHIFT),
              STM32L4_RTC_PRER);
#else /* defined(CONFIG_STM32L4_RTC_LSECLOCK) */
      /* Correct values for 32.768 KHz LSE clock */

      putreg32(((uint32_t)0xff << RTC_PRER_PREDIV_S_SHIFT) |
              ((uint32_t)0x7f << RTC_PRER_PREDIV_A_SHIFT),
              STM32L4_RTC_PRER);
#endif

      /* Exit RTC initialization mode */

      rtc_exitinit();
    }

  /* Re-enable the write protection for RTC registers */

  rtc_wprlock();

  return ret;
}

/************************************************************************************
 * Name: rtc_resume
 *
 * Description:
 *   Called when the RTC was already initialized on a previous power cycle.  This
 *   just brings the RTC back into full operation.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

static void rtc_resume(void)
{
#ifdef CONFIG_RTC_ALARM
  uint32_t regval;

  /* Clear the RTC alarm flags */

  regval  = getreg32(STM32L4_RTC_ISR);
  regval &= ~(RTC_ISR_ALRAF | RTC_ISR_ALRBF);
  putreg32(regval, STM32L4_RTC_ISR);

  /* Clear the EXTI Line 18 Pending bit (Connected internally to RTC Alarm) */

  putreg32((1 << 18), STM32L4_EXTI1_PR);
#endif
}

/************************************************************************************
 * Name: stm32l4_rtc_alarm_handler
 *
 * Description:
 *   RTC ALARM interrupt service routine through the EXTI line
 *
 * Input Parameters:
 *   irq - The IRQ number that generated the interrupt
 *   context - Architecture specific register save information.
 *
 * Returned Value:
 *   Zero (OK) on success; A negated errno value on failure.
 *
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
static int stm32l4_rtc_alarm_handler(int irq, void *context)
{
  FAR struct alm_cbinfo_s *cbinfo;
  alm_callback_t cb;
  FAR void *arg;
  uint32_t isr;
  uint32_t cr;
  int ret = OK;

  isr  = getreg32(STM32L4_RTC_ISR);

  /* Check for EXTI from Alarm A or B and handle according */

  if ((isr & RTC_ISR_ALRAF) != 0)
    {
      cr  = getreg32(STM32L4_RTC_CR);
      if ((cr & RTC_CR_ALRAIE) != 0)
        {
          cbinfo = &g_alarmcb[RTC_ALARMA];
          if (cbinfo->ac_cb != NULL)
            {
              /* Alarm A callback */

              cb  = cbinfo->ac_cb;
              arg = (FAR void *)cbinfo->ac_arg;

              cbinfo->ac_cb  = NULL;
              cbinfo->ac_arg = NULL;

              cb(arg, RTC_ALARMA);
            }

          isr  = getreg32(STM32L4_RTC_ISR) & ~RTC_ISR_ALRAF;
          putreg32(isr, STM32L4_RTC_ISR);
        }
    }

  if ((isr & RTC_ISR_ALRBF) != 0)
    {
      cr  = getreg32(STM32L4_RTC_CR);
      if ((cr & RTC_CR_ALRBIE) != 0)
        {
          cbinfo = &g_alarmcb[RTC_ALARMB];
          if (cbinfo->ac_cb != NULL)
            {
              /* Alarm B callback */

              cb  = cbinfo->ac_cb;
              arg = (FAR void *)cbinfo->ac_arg;

              cbinfo->ac_cb  = NULL;
              cbinfo->ac_arg = NULL;

              cb(arg, RTC_ALARMB);
            }

          isr  = getreg32(STM32L4_RTC_ISR) & ~RTC_ISR_ALRBF;
          putreg32(isr, STM32L4_RTC_ISR);
        }
    }

  return ret;
}
#endif

/************************************************************************************
 * Name: rtchw_check_alrXwf X= a or B
 *
 * Description:
 *   Check registers
 *
 * Input Parameters:
 * None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
static int rtchw_check_alrawf(void)
{
  volatile uint32_t timeout;
  uint32_t regval;
  int ret = -ETIMEDOUT;

  /* Check RTC_ISR ALRAWF for access to alarm register,
   * Can take 2 RTCCLK cycles or timeout
   * CubeMX use GetTick.
   */

  for (timeout = 0; timeout < INITMODE_TIMEOUT; timeout++)
    {
      regval = getreg32(STM32L4_RTC_ISR);
      if ((regval & RTC_ISR_ALRAWF) != 0)
        {
          ret = OK;
          break;
        }
    }

  return ret;
}
#endif

#ifdef CONFIG_RTC_ALARM
static int rtchw_check_alrbwf(void)
{
  volatile uint32_t timeout;
  uint32_t regval;
  int ret = -ETIMEDOUT;

  /* Check RTC_ISR ALRAWF for access to alarm register,
   * can take 2 RTCCLK cycles or timeout
   * CubeMX use GetTick.
   */

  for (timeout = 0; timeout < INITMODE_TIMEOUT; timeout++)
    {
      regval = getreg32(STM32L4_RTC_ISR);
      if ((regval & RTC_ISR_ALRBWF) != 0)
        {
          ret = OK;
          break;
        }
    }

  return ret;
}
#endif

/************************************************************************************
 * Name: stm32_rtchw_set_alrmXr X is a or b
 *
 * Description:
 *   Set the alarm (A or B) hardware registers, using the required hardware access
 *   protocol
 *
 * Input Parameters:
 *   alarmreg - the register
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
static int rtchw_set_alrmar(rtc_alarmreg_t alarmreg)
{
  int ret = -EBUSY;

  /* Need to allow RTC register write
   * Disable the write protection for RTC registers
   */

  rtc_wprunlock();

  /* Disable RTC alarm A & Interrupt A */

  modifyreg32(STM32L4_RTC_CR, (RTC_CR_ALRAE | RTC_CR_ALRAIE), 0);

  ret = rtchw_check_alrawf();
  if (ret != OK)
    {
      goto errout_with_wprunlock;
    }

  /* Set the RTC Alarm A register */

  putreg32(alarmreg, STM32L4_RTC_ALRMAR);
  rtcvdbg("  TR: %08x ALRMAR: %08x\n",
          getreg32(STM32L4_RTC_TR), getreg32(STM32L4_RTC_ALRMAR));

  /* Enable RTC alarm A */

  modifyreg32(STM32L4_RTC_CR, 0, (RTC_CR_ALRAE | RTC_CR_ALRAIE));

errout_with_wprunlock:
  rtc_wprlock();
  return ret;
}
#endif

#ifdef CONFIG_RTC_ALARM
static int rtchw_set_alrmbr(rtc_alarmreg_t alarmreg)
{
  int ret = -EBUSY;

  /* Need to allow RTC register write
   * Disable the write protection for RTC registers
   */

  rtc_wprunlock();

  /* Disable RTC alarm B & Interrupt B */

  modifyreg32(STM32L4_RTC_CR, (RTC_CR_ALRBE | RTC_CR_ALRBIE), 0);

  ret = rtchw_check_alrbwf();
  if (ret != OK)
    {
      goto rtchw_set_alrmbr_exit;
    }

  /* Set the RTC Alarm register */

  putreg32(alarmreg, STM32L4_RTC_ALRMBR);
  rtcvdbg("  TR: %08x ALRMBR: %08x\n",
          getreg32(STM32L4_RTC_TR), getreg32(STM32L4_RTC_ALRMBR));

  /* Enable RTC alarm B */

  modifyreg32(STM32L4_RTC_CR, 0, (RTC_CR_ALRBE | RTC_CR_ALRBIE));

rtchw_set_alrmbr_exit:
  rtc_wprlock();
  return ret;
}
#endif

/************************************************************************************
 * Public Functions
 ************************************************************************************/

/************************************************************************************
 * Name: up_rtc_initialize
 *
 * Description:
 *   Initialize the hardware RTC per the selected configuration.  This function is
 *   called once during the OS initialization sequence
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int up_rtc_initialize(void)
{
  uint32_t regval;
  uint32_t tr_bkp;
  uint32_t dr_bkp;
  int ret;
  int maxretry = 10;
  int nretry = 0;

  /* Clocking for the PWR block must be provided.
   */

  rtc_dumpregs("On reset");

  /* Select the clock source */
  /* Save the token before losing it when resetting */

  regval = getreg32(RTC_MAGIC_REG);

  (void)stm32l4_pwr_enablebkp(true);

  if (regval != RTC_MAGIC)
    {
      /* We might be changing RTCSEL - to ensure such changes work, we must reset the
       * backup domain (having backed up the RTC_MAGIC token)
       */

      modifyreg32(STM32L4_RCC_BDCR, 0, RCC_BDCR_BDRST);
      modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_BDRST, 0);

      /* Some boards do not have the external 32khz oscillator installed, for those
       * boards we must fall back to the crummy internal RC clock or the external high
       * rate clock (which for the STM32L4 must not exceed 4MHz).
       */

#ifdef CONFIG_STM32L4_RTC_HSECLOCK
      /* Use the HSE clock as the input to the RTC block */

      modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_RTCSEL_MASK, RCC_BDCR_RTCSEL_HSE);

#elif defined(CONFIG_STM32L4_RTC_LSICLOCK)
      /* Use the LSI clock as the input to the RTC block */

      modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_RTCSEL_MASK, RCC_BDCR_RTCSEL_LSI);

#elif defined(CONFIG_STM32L4_RTC_LSECLOCK)
      /* Use the LSE clock as the input to the RTC block */

      modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_RTCSEL_MASK, RCC_BDCR_RTCSEL_LSE);

#endif
      /* Enable the RTC Clock by setting the RTCEN bit in the RCC register */

      modifyreg32(STM32L4_RCC_BDCR, 0, RCC_BDCR_RTCEN);
    }
  else /* The RTC is already in use: check if the clock source is changed */
    {
#if defined(CONFIG_STM32L4_RTC_HSECLOCK) || defined(CONFIG_STM32L4_RTC_LSICLOCK) || \
    defined(CONFIG_STM32L4_RTC_LSECLOCK)

      uint32_t clksrc = getreg32(STM32L4_RCC_BDCR);

#if defined(CONFIG_STM32L4_RTC_HSECLOCK)
      if ((clksrc & RCC_BDCR_RTCSEL_MASK) != RCC_BDCR_RTCSEL_HSE)
#elif defined(CONFIG_STM32L4_RTC_LSICLOCK)
      if ((clksrc & RCC_BDCR_RTCSEL_MASK) != RCC_BDCR_RTCSEL_LSI)
#elif defined(CONFIG_STM32L4_RTC_LSECLOCK)
      if ((clksrc & RCC_BDCR_RTCSEL_MASK) != RCC_BDCR_RTCSEL_LSE)
#endif
#endif
        {
          tr_bkp = getreg32(STM32L4_RTC_TR);
          dr_bkp = getreg32(STM32L4_RTC_DR);
          modifyreg32(STM32L4_RCC_BDCR, 0, RCC_BDCR_BDRST);
          modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_BDRST, 0);

#if defined(CONFIG_STM32L4_RTC_HSECLOCK)
          /* Change to the new clock as the input to the RTC block */

          modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_RTCSEL_MASK, RCC_BDCR_RTCSEL_HSE);

#elif defined(CONFIG_STM32L4_RTC_LSICLOCK)
          modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_RTCSEL_MASK, RCC_BDCR_RTCSEL_LSI);

#elif defined(CONFIG_STM32L4_RTC_LSECLOCK)
          modifyreg32(STM32L4_RCC_BDCR, RCC_BDCR_RTCSEL_MASK, RCC_BDCR_RTCSEL_LSE);
#endif

          putreg32(tr_bkp, STM32L4_RTC_TR);
          putreg32(dr_bkp, STM32L4_RTC_DR);

          /* Remember that the RTC is initialized */

          putreg32(RTC_MAGIC, RTC_MAGIC_REG);

          /* Enable the RTC Clock by setting the RTCEN bit in the RCC register */

          modifyreg32(STM32L4_RCC_BDCR, 0, RCC_BDCR_RTCEN);
        }
    }

  (void)stm32l4_pwr_enablebkp(false);

  /* Loop, attempting to initialize/resume the RTC.  This loop is necessary
   * because it seems that occasionally it takes longer to initialize the RTC
   * (the actual failure is in rtc_synchwait()).
   */

  do
    {
      /* Wait for the RTC Time and Date registers to be synchronized with RTC APB
       * clock.
       */

      ret = rtc_synchwait();

      /* Check that rtc_syncwait() returned successfully */

      switch (ret)
        {
          case OK:
            {
              rtclldbg("rtc_syncwait() okay\n");
              break;
            }

          default:
            {
              rtclldbg("rtc_syncwait() failed (%d)\n", ret);
              break;
            }
        }
    }
  while (ret != OK && ++nretry < maxretry);

  /* Check if the one-time initialization of the RTC has already been
   * performed. We can determine this by checking if the magic number
   * has been writing to to back-up date register DR0.
   */

  if (regval != RTC_MAGIC)
    {
      rtclldbg("Do setup\n");

      /* Perform the one-time setup of the LSE clocking to the RTC */

      ret = rtc_setup();

      /* Enable write access to the backup domain (RTC registers, RTC
       * backup data registers and backup SRAM).
       */

      (void)stm32l4_pwr_enablebkp(true);

      /* Remember that the RTC is initialized */

      putreg32(RTC_MAGIC, RTC_MAGIC_REG);
    }
  else
    {
      rtclldbg("Do resume\n");

      /* RTC already set-up, just resume normal operation */

      rtc_resume();
      rtc_dumpregs("Did resume");
    }

  /* Disable write access to the backup domain (RTC registers, RTC backup
   * data registers and backup SRAM).
   */

  (void)stm32l4_pwr_enablebkp(false);

  if (ret != OK && nretry > 0)
    {
      rtclldbg("setup/resume ran %d times and failed with %d\n",
                nretry, ret);
      return -ETIMEDOUT;
    }

#ifdef CONFIG_RTC_ALARM
  /* Configure RTC interrupt to catch alarm interrupts. All RTC interrupts are
   * connected to the EXTI controller.  To enable the RTC Alarm interrupt, the
   * following sequence is required:
   *
   * 1. Configure and enable the EXTI Line 18 in interrupt mode and select the
   *    rising edge sensitivity.
   *    EXTI line 19 RTC Tamper or Timestamp or CSS_LSE
   *    EXTI line 20 RTC Wakeup
   * 2. Configure and enable the RTC_Alarm IRQ channel in the NVIC.
   * 3. Configure the RTC to generate RTC alarms (Alarm A or Alarm B).
   */

  stm32l4_exti_alarm(true, false, true, stm32l4_rtc_alarm_handler);
#endif

  g_rtc_enabled = true;
  rtc_dumpregs("After Initialization");
  return OK;
}

/************************************************************************************
 * Name: stm32l4_rtc_getdatetime_with_subseconds
 *
 * Description:
 *   Get the current date and time from the date/time RTC.  This interface
 *   is only supported by the date/time RTC hardware implementation.
 *   It is used to replace the system timer.  It is only used by the RTOS during
 *   initialization to set up the system time when CONFIG_RTC and CONFIG_RTC_DATETIME
 *   are selected.
 *
 *   Sub-second accuracy is returned through 'nsec'.
 *
 * Input Parameters:
 *   tp - The location to return the high resolution time value.
 *   nsec - The location to return the subsecond time value.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int stm32l4_rtc_getdatetime_with_subseconds(FAR struct tm *tp, FAR long *nsec)
{
  uint32_t ssr;
  uint32_t dr;
  uint32_t tr;
  uint32_t tmp;

  /* Sample the data time registers.  There is a race condition here... If we sample
   * the time just before midnight on December 31, the date could be wrong because
   * the day rolled over while were sampling.
   */

  do
    {
      dr  = getreg32(STM32L4_RTC_DR);
      tr  = getreg32(STM32L4_RTC_TR);
      ssr = getreg32(STM32L4_RTC_SSR);
      tmp = getreg32(STM32L4_RTC_DR);
    }
  while (tmp != dr);

  rtc_dumpregs("Reading Time");

  /* Convert the RTC time to fields in struct tm format.  All of the STM32
   * All of the ranges of values correspond between struct tm and the time
   * register.
   */

  tmp = (tr & (RTC_TR_SU_MASK | RTC_TR_ST_MASK)) >> RTC_TR_SU_SHIFT;
  tp->tm_sec = rtc_bcd2bin(tmp);

  tmp = (tr & (RTC_TR_MNU_MASK | RTC_TR_MNT_MASK)) >> RTC_TR_MNU_SHIFT;
  tp->tm_min = rtc_bcd2bin(tmp);

  tmp = (tr & (RTC_TR_HU_MASK | RTC_TR_HT_MASK)) >> RTC_TR_HU_SHIFT;
  tp->tm_hour = rtc_bcd2bin(tmp);

  /* Now convert the RTC date to fields in struct tm format:
   * Days: 1-31 match in both cases.
   * Month: STM32 is 1-12, struct tm is 0-11.
   * Years: STM32 is 00-99, struct tm is years since 1900.
   * WeekDay: STM32 is 1 = Mon - 7 = Sun
   *
   * Issue:  I am not sure what the STM32 years mean.  Are these the
   * years 2000-2099?  I'll assume so.
   */

  tmp = (dr & (RTC_DR_DU_MASK | RTC_DR_DT_MASK)) >> RTC_DR_DU_SHIFT;
  tp->tm_mday = rtc_bcd2bin(tmp);

  tmp = (dr & (RTC_DR_MU_MASK | RTC_DR_MT)) >> RTC_DR_MU_SHIFT;
  tp->tm_mon = rtc_bcd2bin(tmp) - 1;

  tmp = (dr & (RTC_DR_YU_MASK | RTC_DR_YT_MASK)) >> RTC_DR_YU_SHIFT;
  tp->tm_year = rtc_bcd2bin(tmp) + 100;

#if defined(CONFIG_LIBC_LOCALTIME) || defined(CONFIG_TIME_EXTENDED)
  tmp = (dr & RTC_DR_WDU_MASK) >> RTC_DR_WDU_SHIFT;
  tp->tm_wday = tmp % 7;
  tp->tm_yday = tp->tm_mday + clock_daysbeforemonth(tp->tm_mon, clock_isleapyear(tp->tm_year + 1900));
  tp->tm_isdst = 0
#endif

  /* Return RTC sub-seconds if a non-NULL value
   * of nsec has been provided to receive the sub-second value.
   */

  if (nsec)
    {
      uint32_t prediv_s;
      uint32_t usecs;

      prediv_s   = getreg32(STM32L4_RTC_PRER) & RTC_PRER_PREDIV_S_MASK;
      prediv_s >>= RTC_PRER_PREDIV_S_SHIFT;

      ssr &= RTC_SSR_MASK;

      /* Maximum prediv_s is 0x7fff, thus we can multiply by 100000 and
       * still fit 32-bit unsigned integer.
       */

      usecs = (((prediv_s - ssr) * 100000) / (prediv_s + 1)) * 10;
      *nsec = usecs * 1000;
    }

  rtc_dumptime(tp, "Returning");
  return OK;
}

/************************************************************************************
 * Name: up_rtc_getdatetime
 *
 * Description:
 *   Get the current date and time from the date/time RTC.  This interface
 *   is only supported by the date/time RTC hardware implementation.
 *   It is used to replace the system timer.  It is only used by the RTOS during
 *   initialization to set up the system time when CONFIG_RTC and CONFIG_RTC_DATETIME
 *   are selected.
 *
 *   NOTE: Some date/time RTC hardware is capability of sub-second accuracy.  That
 *   sub-second accuracy is lost in this interface.  However, since the system time
 *   is reinitialized on each power-up/reset, there will be no timing inaccuracy in
 *   the long run.
 *
 * Input Parameters:
 *   tp - The location to return the high resolution time value.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int up_rtc_getdatetime(FAR struct tm *tp)
{
  return stm32l4_rtc_getdatetime_with_subseconds(tp, NULL);
}

/************************************************************************************
 * Name: stm32l4_rtc_setdatetime
 *
 * Description:
 *   Set the RTC to the provided time. RTC implementations which provide
 *   up_rtc_getdatetime() (CONFIG_RTC_DATETIME is selected) should provide this
 *   function.
 *
 * Input Parameters:
 *   tp - the time to use
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int stm32l4_rtc_setdatetime(FAR const struct tm *tp)
{
  uint32_t tr;
  uint32_t dr;
  int ret;

  rtc_dumptime(tp, "Setting time");

  /* Then write the broken out values to the RTC */

  /* Convert the struct tm format to RTC time register fields.  All of the STM32
   * All of the ranges of values correspond between struct tm and the time
   * register.
   */

  tr = (rtc_bin2bcd(tp->tm_sec)  << RTC_TR_SU_SHIFT) |
       (rtc_bin2bcd(tp->tm_min)  << RTC_TR_MNU_SHIFT) |
       (rtc_bin2bcd(tp->tm_hour) << RTC_TR_HU_SHIFT);
  tr &= ~RTC_TR_RESERVED_BITS;

  /* Now convert the fields in struct tm format to the RTC date register fields:
   * Days: 1-31 match in both cases.
   * Month: STM32 is 1-12, struct tm is 0-11.
   * Years: STM32 is 00-99, struct tm is years since 1900.
   * WeekDay: STM32 is 1 = Mon - 7 = Sun
   * Issue:  I am not sure what the STM32 years mean.  Are these the
   * years 2000-2099?  I'll assume so.
   */

  dr = (rtc_bin2bcd(tp->tm_mday) << RTC_DR_DU_SHIFT) |
       ((rtc_bin2bcd(tp->tm_mon + 1))  << RTC_DR_MU_SHIFT) |
#if defined(CONFIG_LIBC_LOCALTIME) || defined(CONFIG_TIME_EXTENDED)
       ((tp->tm_wday == 0 ? 7 : (tp->tm_wday & 7))  << RTC_DR_WDU_SHIFT) |
#endif
       ((rtc_bin2bcd(tp->tm_year - 100)) << RTC_DR_YU_SHIFT);

  dr &= ~RTC_DR_RESERVED_BITS;

  /* Disable the write protection for RTC registers */

  rtc_wprunlock();

  /* Set Initialization mode */

  ret = rtc_enterinit();
  if (ret == OK)
    {
      /* Set the RTC TR and DR registers */

      putreg32(tr, STM32L4_RTC_TR);
      putreg32(dr, STM32L4_RTC_DR);

      /* Exit Initialization mode and wait for the RTC Time and Date
       * registers to be synchronized with RTC APB clock.
       */

      rtc_exitinit();
      ret = rtc_synchwait();
    }

  /* Re-enable the write protection for RTC registers */

  rtc_wprlock();
  rtc_dumpregs("New time setting");
  return ret;
}

/************************************************************************************
 * Name: up_rtc_settime
 *
 * Description:
 *   Set the RTC to the provided time.  All RTC implementations must be able to
 *   set their time based on a standard timespec.
 *
 * Input Parameters:
 *   tp - the time to use
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int up_rtc_settime(FAR const struct timespec *tp)
{
  FAR struct tm newtime;

  /* Break out the time values (not that the time is set only to units of seconds) */

  (void)gmtime_r(&tp->tv_sec, &newtime);
  return stm32l4_rtc_setdatetime(&newtime);
}

/************************************************************************************
 * Name: stm32l4_rtc_setalarm
 *
 * Description:
 *   Set an alarm to an absolute time using associated hardware.
 *
 * Input Parameters:
 *  alminfo - Information about the alarm configuration.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
int stm32l4_rtc_setalarm(FAR struct alm_setalarm_s *alminfo)
{
  FAR struct alm_cbinfo_s *cbinfo;
  rtc_alarmreg_t alarmreg;
  int ret = -EINVAL;

  ASSERT(alminfo != NULL);
  DEBUGASSERT(RTC_ALARM_LAST > alminfo->as_id);

  /* REVISIT:  Should test that the time is in the future */

  rtc_dumptime(&alminfo->as_time, "New alarm time");

  /* Break out the values to the HW alarm register format */

  alarmreg = rtc_reg_alrmr_bin2bcd(&alminfo->as_time);

  /* Set the alarm in hardware and enable interrupts */

  switch (alminfo->as_id)
    {
      case RTC_ALARMA:
        {
          cbinfo         = &g_alarmcb[RTC_ALARMA];
          cbinfo->ac_cb  = alminfo->as_cb;
          cbinfo->ac_arg = alminfo->as_arg;

          ret = rtchw_set_alrmar(alarmreg | RTC_ALRMR_ENABLE);
          if (ret < 0)
            {
              cbinfo->ac_cb  = NULL;
              cbinfo->ac_arg = NULL;
            }
        }
        break;

      case RTC_ALARMB:
        {
          cbinfo         = &g_alarmcb[RTC_ALARMB];
          cbinfo->ac_cb  = alminfo->as_cb;
          cbinfo->ac_arg = alminfo->as_arg;

          ret = rtchw_set_alrmbr(alarmreg | RTC_ALRMR_ENABLE);
          if (ret < 0)
            {
              cbinfo->ac_cb  = NULL;
              cbinfo->ac_arg = NULL;
            }
        }
        break;

      default:
        rtcvdbg("ERROR: Invalid ALARM%d\n", alminfo->as_id);
        break;
    }

  rtc_dumpregs("After alarm setting");

  return ret;
}
#endif

/****************************************************************************
 * Name: stm32l4_rtc_cancelalarm
 *
 * Description:
 *   Cancel an alaram.
 *
 * Input Parameters:
 *  alarmid - Identifies the alarm to be cancelled
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ****************************************************************************/

#ifdef CONFIG_RTC_ALARM
int stm32l4_rtc_cancelalarm(enum alm_id_e alarmid)
{
  int ret = -EINVAL;

  DEBUGASSERT(RTC_ALARM_LAST > alarmid);

  /* Cancel the alarm in hardware and disable interrupts */

  switch (alarmid)
    {
      case RTC_ALARMA:
        {
          /* Cancel the global callback function */

           g_alarmcb[alarmid].ac_cb  = NULL;
           g_alarmcb[alarmid].ac_arg = NULL;

          /* Need to follow RTC register wrote protection.
           * Disable the write protection for RTC registers
           */

          rtc_wprunlock();

          /* Disable RTC alarm and interrupt */

          modifyreg32(STM32L4_RTC_CR, (RTC_CR_ALRAE | RTC_CR_ALRAIE), 0);

          ret = rtchw_check_alrawf();
          if (ret < 0)
            {
              goto errout_with_wprunlock;
            }

          /* Unset the alarm */

          putreg32(-1, STM32L4_RTC_ALRMAR);
          rtc_wprlock();
          ret = OK;
        }
        break;

      case RTC_ALARMB:
    {
          /* Cancel the global callback function */

           g_alarmcb[alarmid].ac_cb  = NULL;
           g_alarmcb[alarmid].ac_arg = NULL;

          /* Need to follow RTC register wrote protection.
           * Disable the write protection for RTC registers
           */

          rtc_wprunlock();

          /* Disable RTC alarm and interrupt */

          modifyreg32(STM32L4_RTC_CR, (RTC_CR_ALRBE | RTC_CR_ALRBIE), 0);

          ret = rtchw_check_alrbwf();
          if (ret < 0)
            {
              goto errout_with_wprunlock;
            }

          /* Unset the alarm */

          putreg32(-1, STM32L4_RTC_ALRMBR);
          rtc_wprlock();
      ret = OK;
    }
        break;

      default:
        rtcvdbg("ERROR: Invalid ALARM%d\n", alarmid);
        break;
    }

  return ret;

errout_with_wprunlock:
  rtc_wprlock();
  return ret;
}
#endif

#endif /* CONFIG_RTC */
