#include "hlw8012.h"
#include "esphome/core/log.h"

namespace esphome {
namespace hlw8012 {

static const char *const TAG = "hlw8012";

// valid for HLW8012 and CSE7759
static const uint32_t HLW8012_CLOCK_FREQUENCY = 3579000;

void HLW8012Component::setup() {
  float reference_voltage = 0;
  ESP_LOGCONFIG(TAG, "Setting up HLW8012...");
  this->sel_pin_->setup();
  this->sel_pin_->digital_write(this->current_mode_);
  this->cf_store_.setup(this->cf_pin_);
  this->cf1_store_.setup(this->cf1_pin_);

  // Initialize multipliers
  if (this->sensor_model_ == HLW8012_SENSOR_MODEL_BL0937) {
    reference_voltage = 1.218f;
    this->power_multiplier_ =
        reference_voltage * reference_voltage * this->voltage_divider_ / this->current_resistor_ / 1721506.0f;
    this->current_multiplier_ = reference_voltage / this->current_resistor_ / 94638.0f;
    this->voltage_multiplier_ = reference_voltage * this->voltage_divider_ / 15397.0f;
  } else {
    // HLW8012 and CSE7759 have same reference specs
    reference_voltage = 2.43f;
    this->power_multiplier_ = reference_voltage * reference_voltage * this->voltage_divider_ / this->current_resistor_ *
                              64.0f / 24.0f / HLW8012_CLOCK_FREQUENCY;
    this->current_multiplier_ = reference_voltage / this->current_resistor_ * 512.0f / 24.0f / HLW8012_CLOCK_FREQUENCY;
    this->voltage_multiplier_ = reference_voltage * this->voltage_divider_ * 256.0f / HLW8012_CLOCK_FREQUENCY;
  }
}
void HLW8012Component::dump_config() {
  ESP_LOGCONFIG(TAG, "HLW8012:");
  LOG_PIN("  SEL Pin: ", this->sel_pin_)
  LOG_PIN("  CF Pin: ", this->cf_pin_)
  LOG_PIN("  CF1 Pin: ", this->cf1_pin_)
  ESP_LOGCONFIG(TAG, "  Change measurement mode every %u", this->change_mode_every_);
  ESP_LOGCONFIG(TAG, "  Current resistor: %.1f mΩ", this->current_resistor_ * 1000.0f);
  ESP_LOGCONFIG(TAG, "  Voltage Divider: %.1f", this->voltage_divider_);
  LOG_UPDATE_INTERVAL(this)
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_)
  LOG_SENSOR("  ", "Current", this->current_sensor_)
  LOG_SENSOR("  ", "Power", this->power_sensor_)
  LOG_SENSOR("  ", "Energy", this->energy_sensor_)
}
float HLW8012Component::get_setup_priority() const { return setup_priority::DATA; }

float HLW8012Component::period_to_power(float period_in) {
    return period_to_hz(period_in) * this->power_multiplier_;
}

float HLW8012Component::period_to_current(float period_in) {
    return period_to_hz(period_in) * this->current_multiplier_;
}

float HLW8012Component::period_to_voltage(float period_in) {
    return period_to_hz(period_in) * this->voltage_multiplier_;
}

float HLW8012Component::period_to_hz(float period_in) {
    if ( period_in == 0.0f ) { return( 0.0f );}
    else                     { return( 1 / (period_in / 1000000.0f) ); }
}

void HLW8012Component::update() {

  if (this->nth_value_ < 2) {
    this->nth_value_++;
    return;
  }

  // execute power block if power sensor exists
  if (this->power_sensor_ != nullptr) {

    // if more than 10 seconds since last rising edge, consider power to be zero.  or if no value exists
    if ( ((micros()-cf_store_.get_last_rise()) > 10000000) ||
           std::isnan(this->power_sensor_->state) ) {
      this->power_sensor_->publish_state(0.0f);
    }

    // if more than update interval, publish as if edge occured now (if already zero, stay there)
    // has to also be greater than last period.  If pulse is taking greater than update interval, need to at least wait that long
    // before trying to fade down.
    else if ( ((micros()-cf_store_.get_last_rise()) > (this->get_update_interval()*1000)) &&
              ((micros()-cf_store_.get_last_rise()) > cf_store_.get_last_period()) &&
              (this->power_sensor_->state != 0.0f)) {
      this->power_sensor_->publish_state(this->period_to_power(micros()-cf_store_.get_last_rise()));
    }

    // otherwise, publish actual value based on last period
    else {
      this->power_sensor_->publish_state(this->period_to_power(cf_store_.get_last_period()));
    }

  }

  // used to block mode change when waiting beyond update interval for reading.  Currently timing out at 10 seconds.
  bool dont_change_mode = false;

  // execute current block if current sensor exists and we are in current mode
  if ( (this->current_sensor_ != nullptr) && this->current_mode_) {

    // if we don't have a valid period value
    if ( !cf1_store_.get_valid() ) {

      // force zero if over 10 seconds
      if ( ((micros()-cf1_store_.get_last_rise()) > 10000000) ||
           std::isnan(this->current_sensor_->state) ) {
        this->current_sensor_->publish_state(0.0f);
      }

      // fade down to zero if less than 10 seconds
      else if ( ((micros()-cf1_store_.get_last_rise()) > (this->get_update_interval()*1000)) &&
                ((micros()-cf1_store_.get_last_rise()) > cf1_store_.get_last_period()) &&
                (this->current_sensor_->state != 0.0f) ) {
        this->current_sensor_->publish_state(this->period_to_current(micros()-cf1_store_.get_last_rise()));
        dont_change_mode = true;
      } else {
        // if pulse width sensor does not have a valid value, don't change mode to give at least 10 seconds
        dont_change_mode = true;
      }
    }
    // otherwise, if valid, publish actual value based on last period
    else {
      this->current_sensor_->publish_state(this->period_to_current(cf1_store_.get_last_period()));
    }

  }

  // execute voltage block if voltage sensor exists and we are in voltage mode
  else if ( (this->voltage_sensor_ != nullptr) && !this->current_mode_) {

    // if we don't have a valid period value
    if ( !cf1_store_.get_valid() ) {

      // force zero if over 10 seconds
      if ( ((micros()-cf1_store_.get_last_rise()) > 10000000) ||
             std::isnan(this->voltage_sensor_->state) ) {
        this->voltage_sensor_->publish_state(0.0f);
      }

      // fade down to zero if less than 10 seconds
      else if ( ((micros()-cf1_store_.get_last_rise()) > (this->get_update_interval()*1000)) &&
                ((micros()-cf1_store_.get_last_rise()) > cf1_store_.get_last_period()) &&
                (this->voltage_sensor_->state != 0.0f) ) {
        this->voltage_sensor_->publish_state(this->period_to_voltage(micros()-cf1_store_.get_last_rise()));
        dont_change_mode = true;
      } else {
        // if pulse width sensor does not have a valid value, don't change mode to give at least 10 seconds
        dont_change_mode = true;
      }
    }
    // otherwise, if valid, publish actual value based on last period
    else {
      this->voltage_sensor_->publish_state(this->period_to_voltage(cf1_store_.get_last_period()));
    }

  }
  // if we get here, mode variable indicates a sensor that doesn't exist.
  else {
    this->change_mode();
    dont_change_mode = true;
  }

  if ( (this->change_mode_at_++ >= this->change_mode_every_) && !dont_change_mode ) {
    this->change_mode();
  }

}

void HLW8012Component::change_mode() {
    this->current_mode_ = !this->current_mode_;
    ESP_LOGV(TAG, "Changing mode to %s mode", this->current_mode_ ? "CURRENT" : "VOLTAGE");
    this->change_mode_at_ = 1;
    this->sel_pin_->digital_write(this->current_mode_);
    this->cf1_store_.reset();
}

}  // namespace hlw8012
}  // namespace esphome
