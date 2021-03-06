#include <SPI.h>
#include "Display.h"

#define DEBUG true

Display::Display(int8_t cs, int8_t rs, int8_t rst, int8_t spi_sck)
{
    // Ensure the SPI clock is on the correct pin
    SPI.setSCK(spi_sck);

    _tft = {new Adafruit_ST7735(cs, rs, rst)};

    // State initialisation
    _tftUpdate = {new TFTUpdate()};
    _tftUpdate->brightness = false;
    _tftUpdate->time = false;
    _tftUpdate->lastUpdate = 0;
    _tftUpdate->needsUpdate = true;
    _tftUpdate->needsBackground = true;

    _pager = {new TFTPager()};
    _pager->mode = 0x00; // Splash/Welcome
    _pager->nextMode = 0x03; // Drive time, etc
//    _pager->nextMode = 0x05; // Status
    _pager->nextWhen = millis() + 2000; // Switch over in ~2 seconds
    _pager->changeModeOnDirection = false;

    _brightnessPWM = 0;

    _voltBuffer = 0;

    for (uint8_t i = 0; i < DISPLAY_MAX_CHANNELS; i++) {
        _channelBars[i] = {new TFTBar()};
        _channelBars[i]->maxValue = 0;
    }

    _tft->initR(INITR_144GREENTAB);   // initialize a ST7735S chip, green tab
    _tft->fillScreen(DISPLAY_BACKGROUND);
}


/**
 * Enable brightness control via PWM output.
 *
 * Call when a PWM pin has been allocated to driving the TFT backlight.
 *
 * Omiting the call will disable brightness control.
 */
void Display::setupBrightness(uint8_t pwm) {
    _brightnessPWM = pwm;
    pinMode(_brightnessPWM, OUTPUT);

    _brightnessBar = {new TFTBar()};
    _brightnessBar->maxValue = 255;
    _brightnessBar->value = _brightness;
    _brightnessBar->colour = ST7735_BLUE;
    uint16_t inset = 3;
    _brightnessBar->x0 = inset;
    _brightnessBar->maxWidth = _tft->width() - (2 * inset);
    _brightnessBar->height = 14;
    _brightnessBar->y0 = _tft->height() - _brightnessBar->height - inset;

    _setBrightness(33);
}

/**
 * Brightness adjustment API
 *
 * Delta is scaled for covering full range quicker.
 */
void Display::changeBrightness(int16_t delta) {
    // TODO: Scale change in brightness based on current value
    // Small LED PWM changes are not noticeable close to 100% duty
    int16_t newBrightness = delta * 2 + ((int16_t )_brightness);
//    Serial.println(newBrightness, HEX);
    if (newBrightness <= 0 && _brightness >= 0) {
        _setBrightness(0);
    } else if (newBrightness >= 255) {
        _setBrightness(255);
    } else {
        _setBrightness((uint8_t) (newBrightness & 0xFF));
    }
}

/**
 * Brightness setting API
 *
 * Value will be clamped to 0<= b <= 255
 */
void Display::setBrightness(int16_t b) {
    // Only when brightness was configured
    if (_brightnessPWM > 0) {
        // Clamp the value from min to max
        if (b < 0) {
            b = 0;
        } else if (b > 0xFF) {
            b = 0xFF;
        }
        _setBrightness((uint8_t) (b & 0xFF));
    }
}

/**
 * Internal brightness
 */
void Display::_setBrightness(uint8_t b) {
    // Only take action if there was a change
    if (b != _brightness) {
        _brightness = b;
        _tftUpdate->brightness = true;
        analogWrite(_brightnessPWM, _brightness);
    }
}

/**
 * Accept input from directional control.
 */
int16_t accumulatedDirection = 0;
void Display::directionInput(int16_t delta) {
    if (_pager->changeModeOnDirection) {
        uint32_t now = millis();

        accumulatedDirection += delta;

        // Limit the rate of mode changing by forcing some time to elapse from the last change
        if (now - _pager->nextWhen > 100 && abs(accumulatedDirection) >= 5) {
            _tftUpdate->needsUpdate = true;
            _pager->nextWhen = now;
            if (_pager->mode == 0 && delta < 0) {
                _pager->nextMode = DISPLAY_MAX_MODES;
            } else {
                _pager->nextMode = _pager->mode + (delta > 0 ? 1 : -1);
            }

            if (_pager->nextMode > DISPLAY_MAX_MODES) {
                _pager->nextMode = 0;
            }
//            Serial.print("Direction Input nexMode=");
//            Serial.println(_pager->nextMode);
            accumulatedDirection = 0;
        } else if (abs(accumulatedDirection) < 5) {
//            Serial.print("Still accumulating direction: ");
//            Serial.println(accumulatedDirection, DEC);
        } else if (now - _pager->nextWhen > 100) {
            // Reset accumulated direction
            accumulatedDirection = 0;
        }
    } else {
        accumulatedDirection = 0;
        changeBrightness(delta);
    }
}

void Display::pressInput(uint8_t button, boolean pressed) {
    switch (button) {
        case 0x01:
#if DEBUG
            Serial.print("Encoder Button: ");
            Serial.println(pressed ? "DOWN" : "UP");
#endif
            _pager->changeModeOnDirection = pressed;
            break;
        default:
#if DEBUG
            Serial.print("Unknown button: ");
            Serial.println(button);
#endif
            break;
    }
}

/**
 * Update or redraw visible elements.
 */
void Display::maintain() {
    uint32_t now = millis();
    // Test mode change if one was scheduled
    if (_pager->nextWhen > 0 && now >= _pager->nextWhen) {
        // Advance to next mode
        _pager->mode = _pager->nextMode;
        _pager->nextWhen = 0;
        _tftUpdate->needsUpdate = true;
        _tftUpdate->needsBackground = true;
//        Serial.print("maintain: changed mode to "); Serial.println(_pager->mode, HEX);
    }

    // Perform drawing tasks once per interval
    if ((now - _tftUpdate->lastUpdate) > DISPLAY_INTERVAL_UPDATE) {
        if (_tftUpdate->needsUpdate) {
            // Draw the current mode; let it determine if actual drawing needs to be done
            switch (_pager->mode) {
                case 0x00: // Splash/welcome/boot
                    splash();
                    break;
                case 0x01: // Menu
                    _modeDrawMenu();
                    break;
                case 0x02: // Volts
                    _modeDrawVolts();
                    break;
                case 0x03: // Channels
                    _modeDrawChannels();
                    break;
                case 0x04: // Channels
                    _modeDrawDrive();
                    break;
                case 0x05: // Status
                    _modeDrawStatus();
                    break;
                default: // Hmmm.. not sure
                    _tft->fillScreen(DISPLAY_BACKGROUND);
                    _tft->setCursor(40, 60);
                    _tft->setTextColor(ST7735_MAGENTA);
                    _tft->setTextSize(2);
                    _tft->println(_pager->mode, DEC);
//                    _pager->nextMode = 0x00;
//                    _pager->nextWhen = millis() + 1000;
                    break;
            }
            _tftUpdate->lastUpdate = now;
            _tftUpdate->needsUpdate = false;
        }

        // Test overlay (global, partial screen elements)
        if (_tftUpdate->brightness) {
            _modeDrawBrightness();
            _tftUpdate->brightness = false;
            // Reset mode back to previous in 1 second
            _pager->nextMode = _pager->mode;
            _pager->nextWhen = now + 1000;
        }

    }
}

void Display::splash() {
    _tft->setTextWrap(false);
    _tft->fillScreen(ST7735_BLACK);
    _tft->setCursor(10, 30);
    _tft->setTextColor(ST7735_YELLOW);
    _tft->setTextSize(2);
    _tft->println("DigiHead");
    _tft->print(_tft->width()); _tft->print(" x "); _tft->print(_tft->height());
}

void Display::_modeDrawBrightness() {
    // Render a bar for brightness
    TFTBar* bar = _brightnessBar;
//    uint8_t maxBrightness = 255;
    double fraction = _brightness;
    fraction /= bar->maxValue;
    _renderBar(_brightnessBar, fraction);
}

void Display::_modeDrawMenu() {
    _tft->fillScreen(DISPLAY_BACKGROUND);
    _tft->drawRect(2, 2, _tft->width()-4, _tft->height()-4, ST7735_CYAN);
}

void Display::setupVolts(RingBuffer *buffer, uint32_t max) {
    _voltBuffer = buffer;
    _voltsBar ={new TFTBar()};

    _voltsBar->maxValue = max;
    _voltsBar->value = 0;
    _voltsBar->colour = ST7735_GREEN;
    uint16_t inset = 3;
    _voltsBar->x0 = inset;
    _voltsBar->maxWidth = _tft->width() - (2 * inset);
    _voltsBar->height = 60;
    _voltsBar->y0 = _tft->height() - 20 - _voltsBar->height - inset;
}

void Display::setupChannel(uint8_t channel, uint32_t max, uint16_t colour) {
    TFTBar *bar = _channelBars[channel];
    bar->maxValue = max;
    bar->value = 0;
    bar->colour = colour;
    uint16_t inset = 3;
    bar->x0 = inset;
    bar->maxWidth = _tft->width() - (2 * inset);
    bar->height = 22;
    bar->y0 = inset + channel * (bar->height + 2);
}

void Display::voltsReady(bool ready) {
    if (ready && _pager->mode == 0x02) {
        _tftUpdate->needsUpdate = true;
    }
}

void Display::setChannel(uint8_t channel, uint32_t value) {
    if (_channelBars[channel]->maxValue > 0) {
        _channelBars[channel]->value = value;
        if (_pager->mode == 0x03) {
            if (!_tftUpdate->needsUpdate) {
                _tftUpdate->needsUpdate = true;
//                if (channel == 2) {
//                    Serial.print("SET[");
//                    Serial.print(channel);
//                    Serial.print("] = 0x");
//                    Serial.print(value, HEX);
//                    Serial.print(" ");
//                    Serial.println(value);
//                }
            }
        }
    }
}

void Display::setElapsed(uint32_t millis) {
    _elapsed = millis;
    if (_pager->mode == 0x04) {
        if (!_tftUpdate->needsUpdate) {
            _tftUpdate->needsUpdate = true;
        }
    }
}

void Display::setGPSData(long lat, long lon, unsigned long course) {
    if (_lastLat != lat || _lastLon != lon) {
        if (_pager->mode == 0x05) {
            if (!_tftUpdate->needsUpdate) {
                _tftUpdate->needsUpdate = true;
            }
        }
        _lastLat = lat;
        _lastLon = lon;
    }
    if (_course / 4500 != course / 4500) {
        _course = course;
        _tftUpdate->needsUpdate = _pager->mode == 0x05;
    }
}

void Display::setTime() {

}

void Display::_modeDrawVolts() {
    if (_tftUpdate->needsBackground) {
        _tft->fillScreen(DISPLAY_BACKGROUND);

        // Border
        _renderBorder(2, ST7735_GREEN);

        // Tick marks; above and below the bar
        uint8_t major = 20;
        uint8_t minor = 5;
        uint16_t majorHeight = 5;
        uint16_t minorHeight = 2;
        uint16_t y0major = _voltsBar->y0 - 1 - majorHeight;
        uint16_t y0minor = _voltsBar->y0 - 1 - minorHeight;
        uint16_t y1 = _voltsBar->y0 + _voltsBar->height + 1;
        for (int16_t w = 0; w <= _voltsBar->maxWidth; w++) {
            if (w % major == 0) {
                _tft->drawFastVLine(_voltsBar->x0 + w, y0major, majorHeight, _voltsBar->colour);
                _tft->drawFastVLine(_voltsBar->x0 + w, y1, majorHeight, _voltsBar->colour);
            } else if (w % minor == 0) {
                _tft->drawFastVLine(_voltsBar->x0 + w, y0minor, minorHeight, _voltsBar->colour);
                _tft->drawFastVLine(_voltsBar->x0 + w, y1, minorHeight, _voltsBar->colour);
            }
        }

        _tftUpdate->needsBackground = false;
    }
    if (_voltBuffer != 0 && !_voltBuffer->isEmpty()) {

        double fraction = _voltBuffer->read();
        fraction /= _voltsBar->maxValue;
        _renderBar(_voltsBar, fraction);
    }
}

const uint8_t textNormalSize = 2;
const uint16_t charwidth = 6 * textNormalSize;

int previousSeconds = -1;
int previousMinutes = -1;
void Display::_modeDrawDrive() {
    if (_tftUpdate->needsBackground) {
        _tft->fillScreen(DISPLAY_BACKGROUND);
        // Border
        _renderBorder(4, ST7735_GREEN);

        previousSeconds = -1;
        previousMinutes = -1;
        _tftUpdate->needsBackground = false;
    }

    // Drive duration
    int16_t x0 = 10;
    int16_t y0 = 30;
//    int millis = _elapsed % 1000;
    int seconds = (_elapsed % 60000) / 1000;
    int minutes = (_elapsed / 60000);
    _tft->setTextWrap(false);
    _tft->setCursor(x0, y0);
    _tft->setTextColor(ST7735_YELLOW, ST7735_BLACK);
    _tft->setTextSize(textNormalSize);
    // Minutes
    uint16_t minutesWidth = 3 * charwidth;
    if (previousMinutes != minutes) {
//        _tft->fillRect(x0, y0, minutesWidth - 1, 8 * textNormalSize, ST7735_BLUE);
        if (minutes < 10) {
            _tft->print(0);
        }
        _tft->print(minutes);
        _tft->print(":");
        previousMinutes = minutes;
    }

    // Seconds
    if (previousSeconds != seconds) {
        _tft->setCursor(x0 + minutesWidth, y0);
//        _tft->fillRect(x0 + minutesWidth, y0, 2 * charwidth, 8 * textNormalSize, ST7735_BLUE);
        if (seconds < 10) {
            _tft->print(0);
        }
        _tft->print(seconds);
        previousSeconds = seconds;
    }
}

void Display::_modeDrawChannels() {
    if (_tftUpdate->needsBackground) {
        _tft->fillScreen(DISPLAY_BACKGROUND);

        // Border
        _renderBorder(3, ST7735_BLUE);

        _tftUpdate->needsBackground = false;
    }

    for (int8_t ch = 0; ch < DISPLAY_MAX_CHANNELS; ch++) {
        TFTBar *channelBar = _channelBars[ch];
        if (channelBar->maxValue != 0) {
            double fraction = channelBar->value;
            fraction /= channelBar->maxValue;
            _renderBar(channelBar, fraction);
        }
    }
}

char const *heading[] = {
              "N ",
                 "NE",
                     " E",
                 "SE",
              "S ",
          "SW",
       "W ",
          "NW"
};

void Display::_modeDrawStatus() {

    if (_tftUpdate->needsBackground) {
        _tft->fillScreen(DISPLAY_BACKGROUND);

        // Border
        _renderBorder(5, ST7735_MAGENTA);

        _tftUpdate->needsBackground = false;
    }

    _tft->setCursor(6,6);
    _tft->setTextWrap(false);
    _tft->setTextColor(ST7735_WHITE, DISPLAY_BACKGROUND);
    _tft->setTextSize(textNormalSize);

    // Date/Time
    // Position
    _tft->println(_lastLat);
    _tft->println(_lastLon);

    // Course
    // 100th of a degree
    unsigned long bearing = _course / 100;
    if (bearing > 360) {
        Serial.print("invalid course: ");
        Serial.println(_course);
    } else {
        _tft->setCursor(18, 50);
        _tft->setTextSize(8);
        _tft->println(heading[bearing / 45]);
    }
}

void Display::_renderBorder(uint8_t width, uint16_t colour) {
    // Border
    for (uint8_t w = 1; w <= width; w++) {
        _tft->drawRect(w, w, _tft->width() - (2 * w), _tft->height() - (2 * w), colour);
    }
}

void Display::_renderBar(TFTBar *bar, double fraction) {
//    Serial.println(fraction);
    // Calc fractional width from max width
    uint16_t width = (fraction < 1.0 ? (uint16_t)(fraction * bar->maxWidth + 0.5) : bar->maxWidth);

    int16_t maxHeight = bar->y0 + bar->height;

    // Blackout
    int16_t blackWidth = bar->maxWidth - width;
    for (int16_t y = bar->y0; y < maxHeight; y++) {
        _tft->drawFastHLine(bar->x0 + width, y, blackWidth, DISPLAY_BACKGROUND);
    }

    // Partial bar
    for (int16_t y = bar->y0; y < maxHeight; y++) {
        _tft->drawFastHLine(bar->x0, y, width, bar->colour);
    }
}