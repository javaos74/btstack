/*
 * Copyright (C) 2011-2013 by Matthias Ringwald
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. This software may not be used in a commercial product
 *    without an explicit license granted by the copyright holder. 
 *
 * THIS SOFTWARE IS PROVIDED BY MATTHIAS RINGWALD AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

//*****************************************************************************
//
// BLE Peripheral Demo
//
//*****************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <termios.h>

#include "btstack-config.h"

#include <btstack/run_loop.h>
#include "debug.h"
#include "btstack_memory.h"
#include "hci.h"
#include "hci_dump.h"

#include "l2cap.h"

#include "sm.h"
#include "att.h"
#include "att_server.h"
#include "gap_le.h"
#include "central_device_db.h"

#define HEARTBEAT_PERIOD_MS 1000

// test profile
#include "profile.h"

///------
static int advertisements_enabled = 0;
static int gap_discoverable = 1;
static int gap_connectable = 1;
static int gap_bondable = 1;

static timer_source_t heartbeat;
static uint8_t counter = 0;
static int update_client = 0;
static int client_configuration = 0;

static void app_run();
static void show_usage();

static void  heartbeat_handler(struct timer *ts){
    // restart timer
    run_loop_set_timer(ts, HEARTBEAT_PERIOD_MS);
    run_loop_add_timer(ts);

    counter++;
    update_client = 1;
    app_run();
} 

static void app_run(){
    if (!update_client) return;
    if (!att_server_can_send()) return;

    int result = -1;
    switch (client_configuration){
        case 0x01:
            printf("Notify value %u\n", counter);
            result = att_server_notify(0x0f, &counter, 1);
            break;
        case 0x02:
            printf("Indicate value %u\n", counter);
            result = att_server_indicate(0x0f, &counter, 1);
            break;
        default:
            return;
    }        
    if (result){
        printf("Error 0x%02x\n", result);
        return;        
    }
    update_client = 0;
}

// write requests
static int att_write_callback(uint16_t handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size, signature_t * signature){
    printf("WRITE Callback, handle %04x\n", handle);
    switch(handle){
        case 0x0010:
            client_configuration = buffer[0];
            printf("Client Configuration set to %u\n", client_configuration);
            break;
        default:
            printf("Value: ");
            hexdump(buffer, buffer_size);
            break;
    }
    return 1;
}

static uint8_t gap_adv_type(){
    if (gap_connectable){
        return 0;
    }
    return 0x03;
}

static int set_adv_params_after_set_adv_enable = 0;

static void app_packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    
    uint8_t adv_data[] = { 02, 01, 05,   03, 02, 0xf0, 0xff }; 

    switch (packet_type) {
            
        case HCI_EVENT_PACKET:
            switch (packet[0]) {
                
                case BTSTACK_EVENT_STATE:
                    // bt stack activated, get started
                    if (packet[2] == HCI_STATE_WORKING) {
                        printf("SM Init completed\n");
                        hci_send_cmd(&hci_le_set_advertising_data, sizeof(adv_data), adv_data);
                    }
                    break;
                
                case HCI_EVENT_LE_META:
                    switch (packet[2]) {
                        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                            advertisements_enabled = 0;

                            // request connection parameter update - test parameters
                            // l2cap_le_request_connection_parameter_update(READ_BT_16(packet, 4), 20, 1000, 100, 100);
                            break;

                        default:
                            break;
                    }
                    break;

                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    // restart advertising if we have been connected before
                    // -> avoid sending advertise enable a second time before command complete was received 
                    if (advertisements_enabled == 0) {
                        hci_send_cmd(&hci_le_set_advertise_enable, 1);
                        advertisements_enabled = 1;
                    }
                    break;
                    
                case HCI_EVENT_COMMAND_COMPLETE:
                    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_advertising_data)){
                         // only needed for BLE Peripheral
                       hci_send_cmd(&hci_le_set_scan_response_data, 10, adv_data);
                       break;
                    }
                    // first init
                    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_scan_response_data)){
                        bd_addr_t null;
                        printf("hci_le_set_advertising_parameters type %u\n", gap_adv_type());
                        hci_send_cmd(&hci_le_set_advertising_parameters,0x0800, 0x0800, gap_adv_type(), 0, 0, &null, 0x07, 0x00);
                        break;
                    }
                    // update
                    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_advertise_enable)){
                        if (!set_adv_params_after_set_adv_enable) break;
                        set_adv_params_after_set_adv_enable = 0;
                        bd_addr_t null;
                        printf("hci_le_set_advertising_parameters type %u\n", gap_adv_type());
                        hci_send_cmd(&hci_le_set_advertising_parameters,0x0800, 0x0800, gap_adv_type(), 0, 0, &null, 0x07, 0x00);
                    }
                    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_advertising_parameters)){
                        if (gap_discoverable != advertisements_enabled){
                            printf("hci_le_set_advertise_enable %u\n", gap_discoverable);
                            hci_send_cmd(&hci_le_set_advertise_enable, gap_discoverable);
                            advertisements_enabled = gap_discoverable;
                        }
                        show_usage();
                        break;
                    }
                    break;

                case SM_PASSKEY_DISPLAY_NUMBER: {
                    // display number
                    sm_event_t * event = (sm_event_t *) packet;
                    printf("GAP Bonding: Display Passkey '%06u\n", event->passkey);
                    break;
                }

                case SM_PASSKEY_DISPLAY_CANCEL: 
                    printf("GAP Bonding: Display cancel\n");
                    break;

                case SM_AUTHORIZATION_REQUEST: {
                    // auto-authorize connection if requested
                    sm_event_t * event = (sm_event_t *) packet;
                    sm_authorization_grant(event->addr_type, event->address);
                    break;
                }
                case ATT_HANDLE_VALUE_INDICATION_COMPLETE:
                    printf("ATT_HANDLE_VALUE_INDICATION_COMPLETE status %u\n", packet[2]);
                    break;

                default:
                    break;
            }
    }
}

void setup(void){
    /// GET STARTED with BTstack ///
    btstack_memory_init();
    run_loop_init(RUN_LOOP_POSIX);
        
    // use logger: format HCI_DUMP_PACKETLOGGER, HCI_DUMP_BLUEZ or HCI_DUMP_STDOUT
    hci_dump_open("/tmp/hci_dump.pklg", HCI_DUMP_PACKETLOGGER);

    // init HCI
    hci_transport_t    * transport = hci_transport_usb_instance();
    hci_uart_config_t  * config    = NULL;
    bt_control_t       * control   = NULL;
    remote_device_db_t * remote_db = (remote_device_db_t *) &remote_device_db_memory;
    hci_init(transport, config, control, remote_db);

    // set up l2cap_le
    l2cap_init();
    
    // setup central device db
    central_device_db_init();

    // setup SM: Display only
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements( SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION); 
    // sm_set_request_security(1);
    // sm_set_encryption_key_size_range(7,15);

    // setup ATT server
    att_server_init(profile_data, NULL, att_write_callback);    
    att_server_register_packet_handler(app_packet_handler);
}

void show_usage(){
    printf("\n--- CLI for LE Peripheral ---\n");
    printf("Status: discoverable %u, connectable %u, bondable %u, advertisements enabled %u \n", gap_discoverable, gap_connectable, gap_bondable, advertisements_enabled);
    printf("---\n");
    printf("b - bondable off\n");
    printf("B - bondable on\n");
    printf("c - connectable off\n");
    printf("C - connectable on\n");
    printf("d - discoverable off\n");
    printf("D - discoverable on\n");
    printf("Ctrl-c - exit\n");
    printf("---\n");
}

void update_advertisements(){
    bd_addr_t null;
    if (!gap_discoverable){
        gap_connectable = 0;
    }
    if (!gap_connectable){
        gap_bondable = 0;
    }
    if (advertisements_enabled){
        set_adv_params_after_set_adv_enable = 1;
        advertisements_enabled = 0;
        printf("hci_le_set_advertise_enable 0\n");
        hci_send_cmd(&hci_le_set_advertise_enable, 0);
        return;
    }

    hci_send_cmd(&hci_le_set_advertising_parameters,0x0800, 0x0800, gap_adv_type(), 0, 0, null, 0x07, 0x00);
}

int  stdin_process(struct data_source *ds){
    char buffer;
    read(ds->fd, &buffer, 1);
    switch (buffer){
        case 'b':
            gap_bondable = 0;
            sm_set_authentication_requirements(SM_AUTHREQ_NO_BONDING);
            show_usage();
            break;
        case 'B':
            gap_bondable = 1;
            sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
            show_usage();
            break;
        case 'c':
            gap_connectable = 0;
            update_advertisements();
            break;
        case 'C':
            gap_connectable = 1;
            update_advertisements();
            break;
        case 'd':
            gap_discoverable = 0;
            update_advertisements();
            break;
        case 'D':
            gap_discoverable = 1;
            update_advertisements();
            break;
        default:
            show_usage();
            break;

    }
    return 0;
}

static data_source_t stdin_source;
void setup_cli(){

    struct termios term = {0};
    if (tcgetattr(0, &term) < 0)
            perror("tcsetattr()");
    term.c_lflag &= ~ICANON;
    term.c_lflag &= ~ECHO;
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &term) < 0)
            perror("tcsetattr ICANON");

    stdin_source.fd = 0; // stdin
    stdin_source.process = &stdin_process;
    run_loop_add_data_source(&stdin_source);
}

int main(void)
{
    setup();

    setup_cli();

    gap_random_address_set_update_period(60000);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_RESOLVABLE);

    // set one-shot timer
    heartbeat.process = &heartbeat_handler;
    run_loop_set_timer(&heartbeat, HEARTBEAT_PERIOD_MS);
    run_loop_add_timer(&heartbeat);

    // turn on!
    hci_power_control(HCI_POWER_ON);
    
    // go!
    run_loop_execute(); 
    
    // happy compiler!
    return 0;
}