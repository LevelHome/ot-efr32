/*******************************************************************************
 * @file
 * @brief SSED application logic.
 *******************************************************************************
 *  Copyright (c) 2023, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************/
// Define module name for Power Manager debugging feature.
#define CURRENT_MODULE_NAME "OPENTHREAD_SAMPLE_APP"

#include <assert.h>
#include <string.h>

#include <common/code_utils.hpp>
#include <common/logging.hpp>
#include <openthread/cli.h>
#include <openthread/dataset_ftd.h>
#include <openthread/instance.h>
#include <openthread/message.h>
#include <openthread/thread.h>
#include <openthread/udp.h>
#include <openthread/platform/logging.h>

#include "sl_button.h"
#include "sl_simple_button.h"
#include "sl_simple_button_instances.h"

#include "sl_component_catalog.h"
#ifdef SL_CATALOG_POWER_MANAGER_PRESENT
#include "sl_power_manager.h"
#endif

// Constants
#define MULTICAST_ADDR "ff03::1"
#define MULTICAST_PORT 123
#define RECV_PORT 234
#define SSED_CSL_PERIOD_SYMBOLS 3125 // units of 10 symbols = 160 us.
#define SSED_CSL_TIMEOUT_SEC 30      // seconds.
#define FTD_MESSAGE "ftd button"
#define SSED_MESSAGE "ssed button"

// Forward declarations
otInstance *otGetInstance(void);
void        ssedReceiveCallback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo);
extern void otSysEventSignalPending(void);

// Variables
static otUdpSocket sSsedSocket;
static bool        sButtonPressed         = false;
static bool        sRxOnIdleButtonPressed = false;
static bool        sAllowSleep            = false;
static bool        sPrintState            = false;

void sleepyInit(void)
{
    otError error;
    otCliOutputFormat("sleepy-demo-ssed starting in EM1 (idle) mode\r\n");
    otCliOutputFormat("Press Button 0 to toggle between EM2 (sleep) and EM1 (idle) modes\r\n");

    otCliOutputFormat("[csl period: %d us.] [csl timeout: %d sec.]\r\n",
                      SSED_CSL_PERIOD_SYMBOLS * 160,
                      SSED_CSL_TIMEOUT_SEC);
    SuccessOrExit(error = otLinkSetCslChannel(otGetInstance(), 15));
    SuccessOrExit(error = otLinkSetCslPeriod(otGetInstance(), SSED_CSL_PERIOD_SYMBOLS));
    SuccessOrExit(error = otLinkSetCslTimeout(otGetInstance(), SSED_CSL_TIMEOUT_SEC));

    otLinkModeConfig config;
    config.mRxOnWhenIdle = 0;
    config.mDeviceType   = 0;
    config.mNetworkData  = 0;
    SuccessOrExit(error = otThreadSetLinkMode(otGetInstance(), config));

exit:
    if (error != OT_ERROR_NONE)
    {
        otCliOutputFormat("Initialization failed with: %d, %s\r\n", error, otThreadErrorToString(error));
    }
    return;
}

/*
 * Callback from sl_ot_is_ok_to_sleep to check if it is ok to go to sleep.
 */
bool efr32AllowSleepCallback(void)
{
    return sAllowSleep;
}

/*
 * Override default network settings, such as panid, so the devices can join a network
 */
void setNetworkConfiguration(void)
{
    static char          aNetworkName[] = "SleepyEFR32";
    otError              error;
    otOperationalDataset aDataset;

    memset(&aDataset, 0, sizeof(otOperationalDataset));

    /*
     * Fields that can be configured in otOperationDataset to override defaults:
     *     Network Name, Mesh Local Prefix, Extended PAN ID, PAN ID, Delay Timer,
     *     Channel, Channel Mask Page 0, Network Key, PSKc, Security Policy
     */
    aDataset.mActiveTimestamp.mSeconds             = 1;
    aDataset.mComponents.mIsActiveTimestampPresent = true;

    /* Set Channel to 15 */
    aDataset.mChannel                      = 15;
    aDataset.mComponents.mIsChannelPresent = true;

    /* Set Pan ID to 2222 */
    aDataset.mPanId                      = (otPanId)0x2222;
    aDataset.mComponents.mIsPanIdPresent = true;

    /* Set Extended Pan ID to C0DE1AB5C0DE1AB5 */
    uint8_t extPanId[OT_EXT_PAN_ID_SIZE] = {0xC0, 0xDE, 0x1A, 0xB5, 0xC0, 0xDE, 0x1A, 0xB5};
    memcpy(aDataset.mExtendedPanId.m8, extPanId, sizeof(aDataset.mExtendedPanId));
    aDataset.mComponents.mIsExtendedPanIdPresent = true;

    /* Set network key to 1234C0DE1AB51234C0DE1AB51234C0DE */
    uint8_t key[OT_NETWORK_KEY_SIZE] =
        {0x12, 0x34, 0xC0, 0xDE, 0x1A, 0xB5, 0x12, 0x34, 0xC0, 0xDE, 0x1A, 0xB5, 0x12, 0x34, 0xC0, 0xDE};
    memcpy(aDataset.mNetworkKey.m8, key, sizeof(aDataset.mNetworkKey));
    aDataset.mComponents.mIsNetworkKeyPresent = true;

    /* Set Network Name to SleepyEFR32 */
    size_t length = strlen(aNetworkName);
    assert(length <= OT_NETWORK_NAME_MAX_SIZE);
    memcpy(aDataset.mNetworkName.m8, aNetworkName, length);
    aDataset.mComponents.mIsNetworkNamePresent = true;

    /* Set the Active Operational Dataset to this dataset */
    error = otDatasetSetActive(otGetInstance(), &aDataset);
    if (error != OT_ERROR_NONE)
    {
        otCliOutputFormat("otDatasetSetActive failed with: %d, %s\r\n", error, otThreadErrorToString(error));
        return;
    }
}

void initUdp(void)
{
    otError    error;
    otSockAddr bindAddr;

    // Initialize bindAddr
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.mPort = RECV_PORT;

    // Open the socket
    error = otUdpOpen(otGetInstance(), &sSsedSocket, ssedReceiveCallback, NULL);
    if (error != OT_ERROR_NONE)
    {
        otCliOutputFormat("SSED failed to open udp socket with: %d, %s\r\n", error, otThreadErrorToString(error));
        return;
    }

    // Bind to the socket. Close the socket if bind fails.
    error = otUdpBind(otGetInstance(), &sSsedSocket, &bindAddr, OT_NETIF_THREAD);
    if (error != OT_ERROR_NONE)
    {
        otCliOutputFormat("SSED failed to bind udp socket with: %d, %s\r\n", error, otThreadErrorToString(error));
        IgnoreReturnValue(otUdpClose(otGetInstance(), &sSsedSocket));
        return;
    }
}

void sl_button_on_change(const sl_button_t *handle)
{
    if (sl_button_get_state(handle) == SL_SIMPLE_BUTTON_PRESSED)
    {
        if (&sl_button_btn0 == handle)
        {
            sRxOnIdleButtonPressed = true;
        }
        else if (&sl_button_btn1 == handle)
        {
            sButtonPressed = true;
        }
        otSysEventSignalPending();
    }
}

#ifdef SL_CATALOG_KERNEL_PRESENT
#define applicationTick sl_ot_rtos_application_tick
#endif

void applicationTick(void)
{
    otMessageInfo messageInfo;
    otMessage    *message = NULL;
    const char   *payload = SSED_MESSAGE;

    if (sPrintState)
    {
        otCliOutputFormat("sleepy-demo-ssed switching to %s mode\r\n", sAllowSleep ? "EM2 (sleep)" : "EM1 (idle)");
        sPrintState = false;
    }

    // Check for BTN0 button press
    if (sRxOnIdleButtonPressed)
    {
        sRxOnIdleButtonPressed = false;
        sAllowSleep            = !sAllowSleep;
        sPrintState            = true;

#if (defined(SL_CATALOG_KERNEL_PRESENT) && defined(SL_CATALOG_POWER_MANAGER_PRESENT))
        if (sAllowSleep)
        {
            sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);
        }
        else
        {
            sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
        }
#endif
    }

    // Check for BTN1 button press
    if (sButtonPressed)
    {
        sButtonPressed = false;

        // Get a message buffer
        VerifyOrExit((message = otUdpNewMessage(otGetInstance(), NULL)) != NULL);

        // Setup messageInfo
        memset(&messageInfo, 0, sizeof(messageInfo));
        SuccessOrExit(otIp6AddressFromString(MULTICAST_ADDR, &messageInfo.mPeerAddr));
        messageInfo.mPeerPort = MULTICAST_PORT;

        // Append the SSED_MESSAGE payload to the message buffer
        SuccessOrExit(otMessageAppend(message, payload, (uint16_t)strlen(payload)));

        // Send the button press message
        SuccessOrExit(otUdpSend(otGetInstance(), &sSsedSocket, message, &messageInfo));

        // Set message pointer to NULL so it doesn't get free'd by this function.
        // otUdpSend() executing successfully means OpenThread has taken ownership
        // of the message buffer.
        message = NULL;
    }

exit:
    if (message != NULL)
    {
        otMessageFree(message);
    }
    return;
}

void ssedReceiveCallback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    OT_UNUSED_VARIABLE(aContext);
    OT_UNUSED_VARIABLE(aMessageInfo);
    uint8_t buf[64];
    int     length;

    // Read the received message's payload
    length      = otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, sizeof(buf) - 1);
    buf[length] = '\0';

    // Check that the payload matches FTD_MESSAGE
    VerifyOrExit(strncmp((char *)buf, FTD_MESSAGE, sizeof(FTD_MESSAGE)) == 0);

    otCliOutputFormat("Message Received: %s\r\n", buf);

exit:
    return;
}
