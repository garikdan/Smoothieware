/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Kernel.h"
#include "LcdBase.h"
#include "Panel.h"
#include "PanelScreen.h"
#include "MainMenuScreen.h"
#include "WatchScreen.h"
#include "nuts_bolts.h"
#include "utils.h"
#include "TemperatureControlPublicAccess.h"
#include "Robot.h"
#include "Conveyor.h"
#include "PlayerPublicAccess.h"
#include "NetworkPublicAccess.h"
#include "PublicData.h"
#include "SwitchPublicAccess.h"
#include "checksumm.h"
#include "TemperatureControlPool.h"
#include "ExtruderPublicAccess.h"
#include "bitmaps.h"


#include <math.h>
#include <string.h>
#include <string>
#include <stdio.h>
#include <algorithm>

using namespace std;

#define extruder_checksum CHECKSUM("extruder")

WatchScreen::WatchScreen()
{
    speed_changed = false;
    issue_change_speed = false;
    ipstr = nullptr;
    update_counts= 0;
}

WatchScreen::~WatchScreen()
{
    delete[] ipstr;
}

void WatchScreen::on_enter()
{
    THEPANEL->lcd->clear();
    THEPANEL->setup_menu(4);
    get_current_status();
    get_current_pos(this->pos);
    get_sd_play_info();
    this->current_speed = lroundf(get_current_speed());
    this->redraw();
    THEPANEL->enter_control_mode(1, 0.5);
    THEPANEL->set_control_value(this->current_speed);

    // enumerate temperature controls
    temp_controllers.clear();
    std::vector<struct pad_temperature> controllers;
    bool ok = PublicData::get_value(temperature_control_checksum, poll_controls_checksum, &controllers);
    if (ok) {
        for (auto &c : controllers) {
            temp_controllers.push_back(c.id);
        }
    }
}

static struct pad_temperature getTemperatures(uint16_t heater_cs)
{
    struct pad_temperature temp;
    PublicData::get_value( temperature_control_checksum, current_temperature_checksum, heater_cs, &temp );
    return temp;
}

void WatchScreen::on_refresh()
{
    // Exit if the button is clicked
    if ( THEPANEL->click() ) {
        THEPANEL->enter_screen(this->parent);
        return;
    }

    // see if speed is being changed
    if (THEPANEL->control_value_change()) {
        this->current_speed = THEPANEL->get_control_value();
        if (this->current_speed < 10) {
            this->current_speed = 10;
            THEPANEL->set_control_value(this->current_speed);
            THEPANEL->reset_counter();
        } else {
            // flag the update to change the speed, we don't want to issue hundreds of M220s
            // but we do want to display the change we are going to make
            this->speed_changed = true; // flag indicating speed changed
            this->redraw();
        }
    }

    // Update Only every 20 refreshes, 1 a second
    update_counts++;
    if ( update_counts % 20 == 0 ) {
        get_sd_play_info();
        get_current_pos(this->pos);
        get_current_status();
        if (this->speed_changed) {
            this->issue_change_speed = true; // trigger actual command to change speed
            this->speed_changed = false;
        } else if (!this->issue_change_speed) { // change still queued
            // read it in case it was changed via M220
            this->current_speed = lroundf(get_current_speed());
            THEPANEL->set_control_value(this->current_speed);
            THEPANEL->reset_counter();
        }

        this->redraw();

        // for LCDs with leds set them according to heater status
        bool bed_on= false, hotend_on= false, is_hot= false;
        uint8_t heon=0, hemsk= 0x01; // bit set for which hotend is on bit0: hotend1, bit1: hotend2 etc
        for(auto id : temp_controllers) {
            struct pad_temperature c= getTemperatures(id);
            if(c.current_temperature > 50) is_hot= true; // anything is hot
            if(c.designator.front() == 'B' && c.target_temperature > 0) bed_on= true;   // bed on/off
            if(c.designator.front() == 'T') { // a hotend by convention
                if(c.target_temperature > 0){
                    hotend_on= true;// hotend on/off (anyone)
                    heon |= hemsk;
                }
                hemsk <<= 1;
            }
        }

        THEPANEL->lcd->setLed(LED_BED_ON, bed_on);
        THEPANEL->lcd->setLed(LED_HOTEND_ON, hotend_on);
        THEPANEL->lcd->setLed(LED_HOT, is_hot);

        THEPANEL->lcd->setLed(LED_FAN_ON, this->fan_state);

        // TODO: Remove this once RRDGLCD supports new graphics functions
        if (THEPANEL->lcd->hasGraphics() && !THEPANEL->lcd->hasFullGraphics()) {
            // display the graphical icons below the status are
            // for (int i = 0; i < 5; ++i) {
            //     THEPANEL->lcd->bltGlyph(i*24, 42, 16, 16, icons, 15, i*24, 0);
            // }
            if(heon&0x01) THEPANEL->lcd->bltGlyph(0, 42, 16, 16, large_icons, 2, 0, 0);
            if(heon&0x02) THEPANEL->lcd->bltGlyph(27, 42, 16, 16, large_icons, 2, 0, 16);
            if(heon&0x04) THEPANEL->lcd->bltGlyph(55, 42, 16, 16, large_icons, 2, 0, 32);

            if (bed_on)
                THEPANEL->lcd->bltGlyph(83, 42, 16, 16, large_icons, 2, 0, 48);

            if(this->fan_state)
                THEPANEL->lcd->bltGlyph(111, 42, 16, 16, large_icons, 2, 0, 64);
        }
    }
}

// queuing gcodes needs to be done from main loop
void WatchScreen::on_main_loop()
{
    if (this->issue_change_speed) {
        this->issue_change_speed = false;
        set_speed();
    }
    PanelScreen::on_main_loop(); // in case any queued commands left
}

// fetch the data we are displaying
void WatchScreen::get_current_status()
{
    // get fan status
    struct pad_switch s;
    bool ok = PublicData::get_value( switch_checksum, fan_checksum, 0, &s );
    if (ok) {
        this->has_fan = true;
        this->fan_state = s.state;
        this->fan_speed = s.value;

    } else {
        // fan probably disabled
        this->has_fan = false;
        this->fan_state = false;
    }
}

// fetch the data we are displaying
float WatchScreen::get_current_speed()
{
    // in percent
    return 6000.0F / THEROBOT->get_seconds_per_minute();
}

void WatchScreen::get_sd_play_info()
{
    void *returned_data;
    bool ok = PublicData::get_value( player_checksum, get_progress_checksum, &returned_data );
    if (ok) {
        struct pad_progress p =  *static_cast<struct pad_progress *>(returned_data);
        this->elapsed_time = p.elapsed_secs;
        this->sd_pcnt_played = p.percent_complete;
        THEPANEL->set_playing_file(p.filename);

    } else {
        this->elapsed_time = 0;
        this->sd_pcnt_played = 0;
    }
}

void WatchScreen::display_menu_line(uint16_t line)
{
    // in menu mode
    switch ( line ) {
        case 0:
        {
            auto& tm= this->temp_controllers;
            if(tm.size() > 0) {
                // only if we detected heaters in config
                int n= 0;
                if(tm.size() > 2) {
                    // more than two temps we need to cycle between them
                    n= update_counts/100; // increments every 5 seconds
                    int ntemps= (tm.size()+1)/2;
                    n= n%ntemps; // which of the pairs of temps to display
                }

                int off= 0;
                for (size_t i = 0; i < 2; ++i) {
                    size_t o= i+(n*2);
                    if(o>tm.size()-1) break;
                    struct pad_temperature temp= getTemperatures(tm[o]);
                    int t= std::min(999, (int)roundf(temp.current_temperature));
                    int tt= roundf(temp.target_temperature);
                    THEPANEL->lcd->setCursor(off, 0); // col, row
                    off += THEPANEL->lcd->printf("%s:%03d/%03d ", temp.designator.substr(0, 2).c_str(), t, tt);
                }

            }else{
                //THEPANEL->lcd->printf("No Heaters");
            }
            break;
        }
        case 1: {
            pad_extruder_t rd;
            if ( THEPANEL->is_extruder_display_enabled() && THEPANEL->is_playing() && PublicData::get_value(extruder_checksum, (void *)&rd)) {
                float extruder_pos = rd.current_position;
                THEPANEL->lcd->printf("E %1.2f", extruder_pos);
                THEPANEL->lcd->setCursor(12, line);
                THEPANEL->lcd->printf("Z%7.2f", this->pos[2]);
            } else {
                THEPANEL->lcd->printf("X%4d Y%4d Z%7.2f", (int)round(this->pos[0]), (int)round(this->pos[1]), this->pos[2]);
            }
            break;
        }
        case 2: THEPANEL->lcd->printf("%3d%%  %02lu:%02lu:%02lu  %3u%%", this->current_speed, this->elapsed_time / 3600, (this->elapsed_time % 3600) / 60, this->elapsed_time % 60, this->sd_pcnt_played); break;
        case 3: THEPANEL->lcd->printf("%19s", this->get_status()); break;
    }
}

const char *WatchScreen::get_status()
{
    if (THEPANEL->hasMessage())
        return THEPANEL->getMessage().c_str();

    if (THEKERNEL->is_halted())
        return "HALTED Reset or M999";

    if (THEPANEL->is_suspended())
        return "Suspended";

    if (THEPANEL->is_playing())
        return THEPANEL->get_playing_file();

    if (!THECONVEYOR->is_idle())
        return "Printing";

    const char *ip = get_network();
    if (ip == NULL) {
        return "Smoothie ready";
    } else {
        return ip;
    }
}

void WatchScreen::set_speed()
{
    send_gcode("M220", 'S', this->current_speed);
}

const char *WatchScreen::get_network()
{
    void *returned_data;

    bool ok = PublicData::get_value( network_checksum, get_ip_checksum, &returned_data );
    if (ok) {
        uint8_t *ipaddr = (uint8_t *)returned_data;
        char buf[20];
        int n = snprintf(buf, sizeof(buf), "IP %d.%d.%d.%d", ipaddr[0], ipaddr[1], ipaddr[2], ipaddr[3]);
        buf[n] = 0;
        if (this->ipstr == nullptr) {
            this->ipstr = new char[n + 1];
        }
        strcpy(this->ipstr, buf);

        return this->ipstr;
    }

    return NULL;
}

void WatchScreen::redraw()
{
    if (THEPANEL->lcd->hasGraphics()) {
        // TODO: Remove this check once additional graphics functions have been implemented for RRDGLCD
        if (THEPANEL->lcd->hasFullGraphics()) {
            // Use the full graphic watch screen on supported displays
            this->draw_graphics();
        } else {
            // Revert to old screen for RRD
            this->refresh_screen(true);
        }
    } else {
        // Use the text based menu system for text only displays
        this->refresh_screen(false);
    }
}

void WatchScreen::draw_graphics()
{
    THEPANEL->lcd->clear();
	THEPANEL->lcd->setCursor(4,7);
	THEPANEL->lcd->printf("%.19s", this->get_status());
    // Print the hotend temperatures
    auto& tm= this->temp_controllers;
    if(tm.size() > 0) {
        for (size_t i = 0; i < tm.size(); i++) {
            struct pad_temperature temp = getTemperatures(tm[i]);
            int t = std::min(999, (int)roundf(temp.current_temperature));
            int tt= roundf(temp.target_temperature);
           if (temp.designator.front() == 'T') {
				THEPANEL->lcd->setCursorPX(3,1);
                THEPANEL->lcd->printf("%03d\xf8", tt);
				THEPANEL->lcd->setCursorPX(3,19);
				THEPANEL->lcd->printf("%03d\xf8", t);
            } else if (temp.designator.front() == 'B') {
				THEPANEL->lcd->setCursorPX(78,1);
                THEPANEL->lcd->printf("%03d\xf8", tt);
				THEPANEL->lcd->setCursorPX(78,19);
				THEPANEL->lcd->printf("%03d\xf8", t);
           }
        }		
    }	
	     // for LCDs with leds set them according to heater status
        bool bed_on= false, hotend_on= false;
        for(auto id : temp_controllers) {
            struct pad_temperature c= getTemperatures(id);
            if(c.designator.front() == 'B' && c.target_temperature > 0) bed_on= true;   // bed on/off
            if(c.designator.front() == 'T') { // a hotend by convention
                if(c.target_temperature > 0){
                    hotend_on= true;// hotend on/off (anyone)
                }
            }			
        }
	
    // Print the fan speed
    if (this->has_fan) {
		THEPANEL->lcd->setCursorPX(105,1);
        THEPANEL->lcd->printf("%3s", this->fan_state? "" : "OFF");
		if(fan_state != 0) {
		THEPANEL->lcd->setCursorPX(102,1);			
		THEPANEL->lcd->printf("%3i%%", (this->fan_speed*100)/255);
		}		
		THEPANEL->lcd->bltGlyph(107, 10, fan_width, fan_height, fan_off_icon);		
    if(this->fan_state)				
        THEPANEL->lcd->bltGlyph(107, 10, fan_width, fan_height, fan_on_icon);
}
		THEPANEL->lcd->bltGlyph(7, 9, hot_width, hot_height, hot_off_icon);
    if (hotend_on)
		THEPANEL->lcd->bltGlyph(7, 9, hot_width, hot_height, hot_on_icon);
	
		THEPANEL->lcd->bltGlyph(80, 9, bed_width, bed_height, bed_off_icon);
    if (bed_on)
        THEPANEL->lcd->bltGlyph(80, 9, bed_width, bed_height, bed_on_icon);
	
		
    
	
        THEPANEL->lcd->drawHLine(40, 48, 84);
        THEPANEL->lcd->drawHLine(40, 54, 84);
        THEPANEL->lcd->drawVLine(40, 48, 7);
        THEPANEL->lcd->drawVLine(124, 48, 7);
        THEPANEL->lcd->drawBox(3, 27, 122, 11);
        //THEPANEL->lcd->drawVLine(2, 22, 12);		
        //THEPANEL->lcd->drawVLine(125, 22, 12);
        THEPANEL->lcd->setCursorPX(11,29);
        THEPANEL->lcd->printf("X%3d Y%3d Z %3.2f", (int)round(this->pos[0]), (int)round(this->pos[1]), this->pos[2]);
        THEPANEL->lcd->drawBox(41, 48, (this->sd_pcnt_played*83)/100, 5);
        THEPANEL->lcd->bltGlyph(50, 40, icon_width, icon_height, time_icon);
        THEPANEL->lcd->setCursor(10,5);
        THEPANEL->lcd->printf("%02u:%02u:%02u", this->elapsed_time / 3600, (this->elapsed_time % 3600) / 60, this->elapsed_time % 60);
        THEPANEL->lcd->bltGlyph(3, 40, icon_width, icon_height, fr_icon);
        THEPANEL->lcd->setCursor(2,5);
        THEPANEL->lcd->printf("%3d%%", this->current_speed);
        THEPANEL->lcd->bltGlyph(3, 48, icon_width, icon_height, flash_icon);
        THEPANEL->lcd->setCursor(2,6);
        THEPANEL->lcd->printf("%3u%%", this->sd_pcnt_played);
        THEPANEL->lcd->bltGlyph(3, 56, icon_width, icon_height, speed_icon);
}