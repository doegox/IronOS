#include "USBPD.h"
#include "configuration.h"
#ifdef POW_PD
#include "BSP_PD.h"
#include "FreeRTOS.h"
#include "Settings.h"
#include "fusb302b.h"
#include "main.hpp"
#include "pd.h"
#include "policy_engine.h"

#ifndef USB_PD_VMAX
#error Max PD Voltage must be defined
#endif

void ms_delay(uint32_t delayms) {
  // Convert ms -> ticks
  TickType_t ticks = delayms / portTICK_PERIOD_MS;

  vTaskDelay(ticks ? ticks : 1); /* Minimum delay = 1 tick */
}
uint32_t get_ms_timestamp() {
  // Convert ticks -> ms
  return xTaskGetTickCount() * portTICK_PERIOD_MS;
}
bool         pdbs_dpm_evaluate_capability(const pd_msg *capabilities, pd_msg *request);
void         pdbs_dpm_get_sink_capability(pd_msg *cap, const bool isPD3);
bool         EPREvaluateCapabilityFunc(const epr_pd_msg *capabilities, pd_msg *request);
FUSB302      fusb((0x22 << 1), fusb_read_buf, fusb_write_buf, ms_delay); // Create FUSB driver
PolicyEngine pe(fusb, get_ms_timestamp, ms_delay, pdbs_dpm_get_sink_capability, pdbs_dpm_evaluate_capability, EPREvaluateCapabilityFunc, USB_PD_EPR_WATTAGE);
int          USBPowerDelivery::detectionState = 0;
bool         haveSeenCapabilityOffer          = false;
uint16_t     requested_voltage_mv             = 0;

/* The current draw when the output is disabled */
#define DPM_MIN_CURRENT PD_MA2PDI(100)

// Start processing
bool USBPowerDelivery::start() {
  if (fusbPresent() && fusb.fusb_setup()) {
    setupFUSBIRQ();
    return true;
  }
  return false;
}
void    USBPowerDelivery::IRQOccured() { pe.IRQOccured(); }
bool    USBPowerDelivery::negotiationHasWorked() { return pe.pdHasNegotiated(); }
uint8_t USBPowerDelivery::getStateNumber() { return pe.currentStateCode(true); }
void    USBPowerDelivery::step() {
  while (pe.thread()) {
  }
}

void USBPowerDelivery::PPSTimerCallback() { pe.TimersCallback(); }
bool USBPowerDelivery::negotiationInProgress() {
  if (USBPowerDelivery::negotiationComplete()) {
    return false;
  }
  if (haveSeenCapabilityOffer) {
    return false;
  }
  return true;
}
bool USBPowerDelivery::negotiationComplete() {
  if (!fusbPresent()) {
    return true;
  }
  return pe.setupCompleteOrTimedOut(getSettingValue(SettingsOptions::PDNegTimeout));
}
bool USBPowerDelivery::fusbPresent() {
  if (detectionState == 0) {
    if (fusb.fusb_read_id()) {
      detectionState = 1;
    }
  }
  return detectionState == 1;
}

bool USBPowerDelivery::isVBUSConnected() {
#if NEEDS_VBUS_PROBE == 1
  static uint8_t state = 0;
  if (state) {
    return state == 1;
  }
  // Dont run if we havent negotiated
  if (!negotiationComplete()) {
    return true;
  }
  if (fusb.isVBUSConnected()) {
    state = 1;
    return true;
  } else {
    state = 2;
    return false;
  }
#else
  return false;
#endif
}
uint32_t  lastCapabilities[11];
uint32_t *USBPowerDelivery::getLastSeenCapabilities() { return lastCapabilities; }

#ifdef POW_EPR
static unsigned int sqrtI(unsigned long sqrtArg) {
  unsigned int  answer, x;
  unsigned long temp;
  if (sqrtArg == 0) {
    return 0; // undefined result
  }
  if (sqrtArg == 1) {
    return 1; // identity
  }
  answer = 0;                           // integer square root
  for (x = 0x8000; x > 0; x = x >> 1) { // 16 bit shift
    answer |= x;                        // possible bit in root
    temp = answer * answer;             //
    if (temp == sqrtArg) {
      break; // exact, found it
    }
    if (temp > sqrtArg) {
      answer ^= x; // too large, reverse bit
    }
  }
  return answer; // approximate root
}
#endif

// parseCapabilitiesArray returns true if a valid capability was found
// caps is the array of capabilities objects
// best* are output references
bool parseCapabilitiesArray(const uint8_t numCaps, uint8_t *bestIndex, uint16_t *bestVoltage, uint16_t *bestCurrent, bool *bestIsPPS, bool *bestIsAVS) {
  // Walk the given capabilities array; and select the best option
  // Given assumption of fixed tip resistance; this can be simplified to highest voltage selection
  *bestIndex   = 0xFF; // Mark unselected
  *bestVoltage = 5000; // Default 5V

  // Fudge of 0.5 ohms to round up a little to account for us always having off periods in PWM
  uint8_t tipResistance = getTipResistanceX10();
  if (getSettingValue(SettingsOptions::USBPDMode) == usbpdMode_t::DEFAULT) {
    tipResistance += 5;
  }
#ifdef MODEL_HAS_DCDC
  // If this device has step down DC/DC inductor to smooth out current spikes
  // We can instead ignore resistance and go for max voltage we can accept; and rely on the DC/DC regulation to keep under current limit
  tipResistance = 255; // (Push to 25.5 ohms to effectively disable this check)
#endif

  for (uint8_t i = 0; i < numCaps; i++) {
    if ((lastCapabilities[i] & PD_PDO_TYPE) == PD_PDO_TYPE_FIXED) {
      // This is a fixed PDO entry
      // Evaluate if it can produve sufficient current based on the TIP_RESISTANCE (ohms*10)
      // V=I*R -> V/I => minimum resistance, if our tip resistance is >= this then we can use this supply

      int voltage_mv             = PD_PDV2MV(PD_PDO_SRC_FIXED_VOLTAGE_GET(lastCapabilities[i])); // voltage in mV units
      int current_a_x100         = PD_PDO_SRC_FIXED_CURRENT_GET(lastCapabilities[i]);            // current in 10mA units
      int min_resistance_ohmsx10 = voltage_mv / current_a_x100;
      if (voltage_mv > 0) {
        if (voltage_mv <= (USB_PD_VMAX * 1000)) {
          if (min_resistance_ohmsx10 <= tipResistance) {
            // This is a valid power source we can select as
            if (voltage_mv > *bestVoltage) {

              // Higher voltage and valid, select this instead
              *bestIndex   = i;
              *bestVoltage = voltage_mv;
              *bestCurrent = current_a_x100;
              *bestIsPPS   = false;
              *bestIsAVS   = false;
            }
          }
        }
      }
    } else if ((lastCapabilities[i] & PD_PDO_TYPE) == PD_PDO_TYPE_AUGMENTED && getSettingValue(SettingsOptions::USBPDMode)) {
      bool sourceIsEPRCapable = lastCapabilities[0] & PD_PDO_SRC_FIXED_EPR_CAPABLE;
      bool isPPS              = false;
      bool isAVS              = false;
      if (sourceIsEPRCapable) {
        isPPS = (lastCapabilities[i] & PD_APDO_TYPE) == PD_APDO_TYPE_PPS;
        isAVS = (lastCapabilities[i] & PD_APDO_TYPE) == PD_APDO_TYPE_AVS;
      } else {
        isPPS = true; // Assume PPS if no EPR support
      }
      if (isPPS) {
        // If this is a PPS slot, calculate the max voltage in the PPS range that can we be used and maintain
        uint16_t max_voltage = PD_PAV2MV(PD_APDO_PPS_MAX_VOLTAGE_GET(lastCapabilities[i]));
        // uint16_t min_voltage = PD_PAV2MV(PD_APDO_PPS_MIN_VOLTAGE_GET(lastCapabilities[i]));
        uint16_t max_current = PD_PAI2CA(PD_APDO_PPS_CURRENT_GET(lastCapabilities[i])); // max current in 10mA units
        // Using the current and tip resistance, calculate the ideal max voltage
        // if this is range, then we will work with this voltage
        // if this is not in range; then max_voltage can be safely selected
        int ideal_voltage_mv = (tipResistance * max_current);
        if (ideal_voltage_mv > max_voltage) {
          ideal_voltage_mv = max_voltage; // constrain to what this PDO offers
        }
        if (ideal_voltage_mv > 20000) {
          ideal_voltage_mv = 20000; // Limit to 20V as some advertise 21 but are not stable at 21
        }
        if (ideal_voltage_mv > (USB_PD_VMAX * 1000)) {
          ideal_voltage_mv = (USB_PD_VMAX * 1000); // constrain to model max voltage safe to select
        }
        if (ideal_voltage_mv > *bestVoltage) {
          *bestIndex   = i;
          *bestVoltage = ideal_voltage_mv;
          *bestCurrent = max_current;
          *bestIsPPS   = true;
          *bestIsAVS   = false;
        }
      }
#ifdef POW_EPR
      else if (isAVS) {
        uint16_t max_voltage = PD_PAV2MV(PD_APDO_AVS_MAX_VOLTAGE_GET(lastCapabilities[i]));
        uint8_t  max_wattage = PD_APDO_AVS_MAX_POWER_GET(lastCapabilities[i]);

        // W = v^2/tip_resistance => Wattage*tip_resistance == Max_voltage^2
        auto ideal_max_voltage = sqrtI((max_wattage * tipResistance) / 10) * 1000;
        if (ideal_max_voltage > (USB_PD_VMAX * 1000)) {
          ideal_max_voltage = (USB_PD_VMAX * 1000); // constrain to model max voltage safe to select
        }
        if (ideal_max_voltage > (max_voltage)) {
          ideal_max_voltage = (max_voltage); // constrain to model max voltage safe to select
        }
        auto operating_current = (ideal_max_voltage / tipResistance); // Current in centiamps

        if (ideal_max_voltage > *bestVoltage) {
          *bestIndex   = i;
          *bestVoltage = ideal_max_voltage;
          *bestCurrent = operating_current;
          *bestIsAVS   = true;
          *bestIsPPS   = false;
        }
      }
#endif
    }
  }
  // Now that the best index is known, set the current values
  return *bestIndex != 0xFF; // have we selected one
}

bool EPREvaluateCapabilityFunc(const epr_pd_msg *capabilities, pd_msg *request) {
#ifdef POW_EPR
  // Select any EPR slots up to USB_PD_VMAX
  memset(lastCapabilities, 0, sizeof(lastCapabilities));
  memcpy(lastCapabilities, capabilities->obj, sizeof(lastCapabilities));
  // PDO slots 1-7 shall be the standard PDO's
  // PDO slots 8-11 shall be the >20V slots
  uint8_t  numobj           = 11;
  uint8_t  bestIndex        = 0xFF;
  uint16_t bestIndexVoltage = 0;
  uint16_t bestIndexCurrent = 0;
  bool     bestIsPPS        = false;
  bool     bestIsAVS        = false;

  if (parseCapabilitiesArray(numobj, &bestIndex, &bestIndexVoltage, &bestIndexCurrent, &bestIsPPS, &bestIsAVS)) {
    /* We got what we wanted, so build a request for that */
    request->hdr    = PD_MSGTYPE_EPR_REQUEST | PD_NUMOBJ(2);
    request->obj[1] = lastCapabilities[bestIndex]; // Copy PDO into slot 2

    if (bestIsAVS) {
      request->obj[0] = PD_RDO_PROG_CURRENT_SET(PD_CA2PAI(bestIndexCurrent)) | PD_RDO_PROG_VOLTAGE_SET(PD_MV2APS(bestIndexVoltage));
    } else if (bestIsPPS) {
      request->obj[0] = PD_RDO_PROG_CURRENT_SET(PD_CA2PAI(bestIndexCurrent)) | PD_RDO_PROG_VOLTAGE_SET(PD_MV2PRV(bestIndexVoltage));
    } else {
      request->obj[0] = PD_RDO_FV_MAX_CURRENT_SET(bestIndexCurrent) | PD_RDO_FV_CURRENT_SET(bestIndexCurrent);
    }
    request->obj[0] |= PD_RDO_EPR_CAPABLE;
    request->obj[0] |= PD_RDO_NO_USB_SUSPEND;
    request->obj[0] |= PD_RDO_OBJPOS_SET(bestIndex + 1);

    // We dont do usb
    // request->obj[0] |= PD_RDO_USB_COMMS;

    /* Update requested voltage */
    requested_voltage_mv    = bestIndexVoltage;
    powerSupplyWattageLimit = bestIndexVoltage * bestIndexCurrent / 100 / 1000; // Set watts for limit from PSU limit

  } else {
    /* Nothing matched (or no configuration), so get 5 V at low current */
    request->hdr    = PD_MSGTYPE_EPR_REQUEST | PD_NUMOBJ(2);
    request->obj[1] = lastCapabilities[0];
    request->obj[0] = PD_RDO_FV_MAX_CURRENT_SET(100) | PD_RDO_FV_CURRENT_SET(100) | PD_RDO_NO_USB_SUSPEND | PD_RDO_OBJPOS_SET(1);
    // We dont do usb
    // request->obj[0] |= PD_RDO_USB_COMMS;

    /* Update requested voltage */
    requested_voltage_mv = 5000;
  }
  return true;
#endif
  return false;
}

bool pdbs_dpm_evaluate_capability(const pd_msg *capabilities, pd_msg *request) {
  memset(lastCapabilities, 0, sizeof(lastCapabilities));
  memcpy(lastCapabilities, capabilities->obj, sizeof(uint32_t) * 7);
  haveSeenCapabilityOffer = true;
  /* Get the number of PDOs */
  uint8_t numobj = PD_NUMOBJ_GET(capabilities);

  /* Make sure we have configuration */
  /* Look at the PDOs to see if one matches our desires */
  // Look against USB_PD_Desired_Levels to select in order of preference
  uint8_t  bestIndex        = 0xFF;
  uint16_t bestIndexVoltage = 0;
  uint16_t bestIndexCurrent = 0;
  bool     bestIsPPS        = false;
  bool     bestIsAVS        = false;

  if (parseCapabilitiesArray(numobj, &bestIndex, &bestIndexVoltage, &bestIndexCurrent, &bestIsPPS, &bestIsAVS)) {
    /* We got what we wanted, so build a request for that */
    request->hdr = PD_MSGTYPE_REQUEST | PD_NUMOBJ(1);
    if (bestIsPPS) {
      request->obj[0] = PD_RDO_PROG_CURRENT_SET(PD_CA2PAI(bestIndexCurrent)) | PD_RDO_PROG_VOLTAGE_SET(PD_MV2PRV(bestIndexVoltage)) | PD_RDO_NO_USB_SUSPEND | PD_RDO_OBJPOS_SET(bestIndex + 1);
    } else {
      request->obj[0] = PD_RDO_FV_MAX_CURRENT_SET(bestIndexCurrent) | PD_RDO_FV_CURRENT_SET(bestIndexCurrent) | PD_RDO_NO_USB_SUSPEND | PD_RDO_OBJPOS_SET(bestIndex + 1);
    }
    // We dont do usb
    // request->obj[0] |= PD_RDO_USB_COMMS;
#ifdef POW_EPR
    request->obj[0] |= PD_RDO_EPR_CAPABLE;
#endif

    /* Update requested voltage */
    requested_voltage_mv    = bestIndexVoltage;
    powerSupplyWattageLimit = bestIndexVoltage * bestIndexCurrent / 100 / 1000; // Set watts for limit from PSU limit

  } else {
    /* Nothing matched (or no configuration), so get 5 V at low current */
    request->hdr    = PD_MSGTYPE_REQUEST | PD_NUMOBJ(1);
    request->obj[0] = PD_RDO_FV_MAX_CURRENT_SET(100) | PD_RDO_FV_CURRENT_SET(100) | PD_RDO_NO_USB_SUSPEND | PD_RDO_OBJPOS_SET(1);
    // We dont do usb
    // request->obj[0] |= PD_RDO_USB_COMMS;

    /* Update requested voltage */
    requested_voltage_mv = 5000;
  }
  // Even if we didnt match, we return true as we would still like to handshake on 5V at the minimum
  return true;
}

void add_v_record(pd_msg *cap, uint16_t voltage_mv, int numobj) {

  uint16_t current = (voltage_mv) / getTipResistanceX10(); // In centi-amps

  /* Add a PDO for the desired power. */
  cap->obj[numobj] = PD_PDO_TYPE_FIXED | PD_PDO_SNK_FIXED_VOLTAGE_SET(PD_MV2PDV(voltage_mv)) | PD_PDO_SNK_FIXED_CURRENT_SET(current);
}
void pdbs_dpm_get_sink_capability(pd_msg *cap, const bool isPD3) {
  /* Keep track of how many PDOs we've added */
  int numobj = 0;

  /* If we have no configuration or want something other than 5 V, add a PDO
   * for vSafe5V */
  /* Minimum current, 5 V, and higher capability. */
  cap->obj[numobj++] = PD_PDO_TYPE_FIXED | PD_PDO_SNK_FIXED_VOLTAGE_SET(PD_MV2PDV(5000)) | PD_PDO_SNK_FIXED_CURRENT_SET(DPM_MIN_CURRENT);
  // Voltages must be in order of lowest -> highest
#if USB_PD_VMAX >= 20
  add_v_record(cap, 9000, numobj);
  numobj++;
  add_v_record(cap, 15000, numobj);
  numobj++;
  add_v_record(cap, 20000, numobj);
  numobj++;
#elif USB_PD_VMAX >= 15
  add_v_record(cap, 9000, numobj);
  numobj++;
  add_v_record(cap, 12000, numobj);
  numobj++;
  add_v_record(cap, 15000, numobj);
  numobj++;
#elif USB_PD_VMAX >= 12
  add_v_record(cap, 9000, numobj);
  numobj++;
  add_v_record(cap, 12000, numobj);
  numobj++;
#elif USB_PD_VMAX >= 9
  add_v_record(cap, 9000, numobj);
  numobj++;
#endif

  /* Set the USB communications capable flag. */
  cap->obj[0] |= PD_PDO_SNK_FIXED_USB_COMMS;

  /* Set the Sink_Capabilities message header */
  cap->hdr = PD_DATAROLE_UFP | PD_SPECREV_3_0 | PD_POWERROLE_SINK | PD_MSGTYPE_SINK_CAPABILITIES | PD_NUMOBJ(numobj);
}

#endif
