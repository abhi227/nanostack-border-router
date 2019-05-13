/*
 * Copyright (c) 2016 ARM Limited. All rights reserved.
 */

#include <string.h>


#include "mbed.h"
#include "borderrouter_tasklet.h"
#include "drivers/eth_driver.h"
#include "sal-stack-nanostack-slip/Slip.h"

#include "Nanostack.h"
#include "NanostackEthernetInterface.h"
#include "MeshInterfaceNanostack.h"
#include "EMACInterface.h"
#include "EMAC.h"

#ifdef  MBED_CONF_APP_DEBUG_TRACE
#if MBED_CONF_APP_DEBUG_TRACE == 1
#define APP_TRACE_LEVEL TRACE_ACTIVE_LEVEL_DEBUG
#else
#define APP_TRACE_LEVEL TRACE_ACTIVE_LEVEL_INFO
#endif
#endif //MBED_CONF_APP_DEBUG_TRACE

#include "ns_hal_init.h"
#include "mesh_system.h"
#include "cmsis_os.h"
#include "arm_hal_interrupt.h"


#include "mbed_trace.h"
#define TRACE_GROUP "app"
#define RETRY_COUNT 3

#define APP_DEFINED_HEAP_SIZE MBED_CONF_APP_HEAP_SIZE
static uint8_t app_stack_heap[APP_DEFINED_HEAP_SIZE];
static mem_stat_t heap_info;

#define BOARD 1
#define CONFIG 2
#if MBED_CONF_APP_BACKHAUL_MAC_SRC == BOARD
static uint8_t mac[6];
#elif MBED_CONF_APP_BACKHAUL_MAC_SRC == CONFIG
static const uint8_t mac[] = MBED_CONF_APP_BACKHAUL_MAC;
#else
#error "MAC address not defined"
#endif

static DigitalOut led1(MBED_CONF_APP_LED);

static Ticker led_ticker;
static phy_device_driver_s phy;
static uint8_t mac_addr[6];
static int8_t ppp_device_id;

#if MBED_CONF_APP_SOCK_TYPE == TCP
    TCPSocket sock;
#else
    UDPSocket sock;
#endif

const char *host_name = "echo.mbedcloudtesting.com"; //testing


static void app_heap_error_handler(heap_fail_t event);

static void toggle_led1()
{
    led1 = !led1;
}

/**
 * \brief Prints string to serial (adds CRLF).
 */
static void trace_printer(const char *str)
{
    printf("%s\n", str);
}

#undef ETH
#undef SLIP
#undef EMAC
#undef CELL
#define ETH 1
#define SLIP 2
#define EMAC 3
#define CELL 4
#if MBED_CONF_APP_BACKHAUL_DRIVER == EMAC
static void (*emac_actual_cb)(uint8_t, int8_t);
static int8_t emac_driver_id;
static void emac_link_cb(bool up)
{
    if (emac_actual_cb) {
        emac_actual_cb(up, emac_driver_id);
    }
}
#endif

/**
 * Connects to the Cellular Network
 */

static int8_t arm_ppp_phy_address_write(phy_address_type_e address_type, uint8_t *address_ptr)
{
    int8_t retval = 0;

    switch(address_type){
        case PHY_MAC_48BIT:
            //k64f_eth_set_address(address_ptr);
            break;
        case PHY_MAC_64BIT:
        case PHY_MAC_16BIT:
        case PHY_MAC_PANID:
            retval=-1;
            break;
    }

    return retval;
}
/* TODO State Control Handling.*/
static int8_t arm_ppp_phy_interface_state_control(phy_interface_state_e state, uint8_t not_required)
{
    switch(state){
        case PHY_INTERFACE_DOWN:
            break;
        case PHY_INTERFACE_UP:
            break;
        case PHY_INTERFACE_RESET:
            break;
        case PHY_INTERFACE_SNIFFER_STATE:
            /*TODO Allow promiscuous state here*/
            break;
        case PHY_INTERFACE_RX_ENERGY_STATE:
            /*Just to get rid of compiler warning*/
            break;

    }

    (void)not_required;

    return 0;
}
static int8_t arm_ppp_phy_tx(uint8_t *data_ptr, uint16_t data_len, uint8_t tx_handle,data_protocol_e data_flow)
{
    int retval = -1;

/*    if(data_len >= ENET_HDR_LEN){
        retval = k64f_eth_send(data_ptr, data_len);
    }*/

    (void)data_flow;
    (void)tx_handle;

    return retval;
}

nsapi_error_t connect_cellular(NetworkInterface *net)
{
    nsapi_error_t retcode = NSAPI_ERROR_OK;
    uint8_t retry_counter = 0;

    while (net->get_connection_status() != NSAPI_STATUS_GLOBAL_UP) {
        retcode = net->connect();
        if (retcode == NSAPI_ERROR_AUTH_FAILURE) {
        	tr_error("\n\nAuthentication Failure. Exiting application\n");
        } else if (retcode == NSAPI_ERROR_OK) {
        	tr_info("\n\nConnection Established.\n");
        } else if (retry_counter > RETRY_COUNT) {
        	tr_error("\n\nFatal connection failure: %d\n", retcode);
        } else {
        	tr_error("\n\nCouldn't connect: %d, will retry\n", retcode);
            retry_counter++;
            continue;
        }
        break;
    }

/*    while (NULL == net->get_ip_address())
    	ThisThread::sleep_for(500);
    tr_info("Connected. IP = %s\n\n", net->get_ip_address());*/
    return retcode;
}

nsapi_error_t open_ppp_socket(NetworkInterface *net)
{
	nsapi_error_t retcode = sock.open(net);

    if (retcode != NSAPI_ERROR_OK) {
#if MBED_CONF_APP_SOCK_TYPE == TCP
    	tr_error("TCPSocket.open() fails, code: %d\n", retcode);
#else
    	tr_error("UDPSocket.open() fails, code: %d\n", retcode);
#endif
        return -1;
    }

    SocketAddress sock_addr;
    retcode = net->gethostbyname(host_name, &sock_addr);
    if (retcode != NSAPI_ERROR_OK) {
    	tr_error("Couldn't resolve remote host: %s, code: %d\n", host_name, retcode);
        return -1;
    }

    sock_addr.set_port(7);

    sock.set_timeout(15000);
    int n = 0;
    const char *echo_string = "TEST";
    char recv_buf[4];
#if MBED_CONF_APP_SOCK_TYPE == TCP
    retcode = sock.connect(sock_addr);
    if (retcode < 0) {
    	tr_error("TCPSocket.connect() fails, code: %d\n", retcode);
        return -1;
    } else {
    	tr_error("TCP: connected with %s server\n", host_name);
    }
    retcode = sock.send((void*) echo_string, sizeof(echo_string));
    if (retcode < 0) {
    	tr_error("TCPSocket.send() fails, code: %d\n", retcode);
        return -1;
    } else {
    	tr_info("TCP: Sent %d Bytes to %s\n", retcode, host_name);
    }

    n = sock.recv((void*) recv_buf, sizeof(recv_buf));
#else

    retcode = sock.sendto(sock_addr, (void*) echo_string, sizeof(echo_string));
    if (retcode < 0) {
    	tr_error("UDPSocket.sendto() fails, code: %d\n", retcode);
        return -1;
    } else {
    	tr_info("UDP: Sent %d Bytes to %s\n", retcode, host_name);
    }

    n = sock.recvfrom(&sock_addr, (void*) recv_buf, sizeof(recv_buf));
#endif

    sock.close();

    if (n > 0) {
    	tr_info("Received from echo server %d Bytes\n", n);
        return 0;
    }

    return -1;
    return retcode;
}

/**
 * \brief Initializes the MAC backhaul driver.
 * This function is called by the border router module.
 */
void backhaul_driver_init(void (*backhaul_driver_status_cb)(uint8_t, int8_t))
{
// Values allowed in "backhaul-driver" option
#if MBED_CONF_APP_BACKHAUL_DRIVER == SLIP
    SlipMACDriver *pslipmacdriver;
    int8_t slipdrv_id = -1;
#if defined(MBED_CONF_APP_SLIP_HW_FLOW_CONTROL)
    pslipmacdriver = new SlipMACDriver(SERIAL_TX, SERIAL_RX, SERIAL_RTS, SERIAL_CTS);
#else
    pslipmacdriver = new SlipMACDriver(SERIAL_TX, SERIAL_RX);
#endif

    if (pslipmacdriver == NULL) {
        tr_error("Unable to create SLIP driver");
        return;
    }

    tr_info("Using SLIP backhaul driver...");

#ifdef MBED_CONF_APP_SLIP_SERIAL_BAUD_RATE
    slipdrv_id = pslipmacdriver->Slip_Init(mac, MBED_CONF_APP_SLIP_SERIAL_BAUD_RATE);
#else
    tr_warning("baud rate for slip not defined");
#endif

    if (slipdrv_id >= 0) {
        backhaul_driver_status_cb(1, slipdrv_id);
        return;
    }

    tr_error("Backhaul driver init failed, retval = %d", slipdrv_id);
#elif MBED_CONF_APP_BACKHAUL_DRIVER == EMAC
#undef EMAC
    tr_info("Using EMAC backhaul driver...");
    NetworkInterface *net = NetworkInterface::get_default_instance();
    if (!net) {
        tr_error("Default network interface not found");
        exit(1);
    }
    EMACInterface *emacif = net->emacInterface();
    if (!emacif) {
        tr_error("Default interface is not EMAC-based");
        exit(1);
    }
    EMAC &emac = emacif->get_emac();
    Nanostack::EthernetInterface *ns_if;
#if MBED_CONF_APP_BACKHAUL_MAC_SRC == BOARD
    /* Let the core code choose address - either from board or EMAC (for
     * ETH and SLIP we pass in the board address already in mac[]) */
    nsapi_error_t err = Nanostack::get_instance().add_ethernet_interface(emac, true, &ns_if);
    /* Read back what they chose into our mac[] */
    ns_if->get_mac_address(mac);
#else
    nsapi_error_t err = Nanostack::get_instance().add_ethernet_interface(emac, true, &ns_if, mac);
#endif
    if (err < 0) {
        tr_error("Backhaul driver init failed, retval = %d", err);
    } else {
        emac_actual_cb = backhaul_driver_status_cb;
        emac_driver_id = ns_if->get_driver_id();
        emac.set_link_state_cb(emac_link_cb);
    }
#elif MBED_CONF_APP_BACKHAUL_DRIVER == CELL
    tr_info("Using CELLULAR backhaul driver...");
    NetworkInterface *net = NetworkInterface::get_default_instance();
    if (!net) {
        tr_error("Default network interface not found");
        exit(1);
    }

    phy.PHY_MAC = mac_addr;
    phy.address_write = arm_ppp_phy_address_write;
    phy.driver_description = const_cast<char *>("PPP");
    phy.link_type = PHY_LINK_SLIP;
    phy.phy_MTU = 0;
    phy.phy_header_length = 0;
    phy.phy_tail_length = 0;
    phy.state_control = arm_ppp_phy_interface_state_control;
    phy.tx = arm_ppp_phy_tx;
    phy.phy_rx_cb = NULL;
    phy.phy_tx_done_cb = NULL;

    ppp_device_id = arm_net_phy_register(&phy);
    backhaul_driver_status_cb(1, ppp_device_id);
    connect_cellular(net);
    open_ppp_socket(net);
    //nsapi_error_t err = Nanostack::get_instance().add_ethernet_interface(emac, true, &ns_if);
#elif MBED_CONF_APP_BACKHAUL_DRIVER == ETH
    tr_info("Using ETH backhaul driver...");
    arm_eth_phy_device_register(mac, backhaul_driver_status_cb);
    return;
#else
#error "Unsupported backhaul driver"
#endif
#undef SLIP
#undef ETH
#undef EMAC
}


void appl_info_trace(void)
{
    tr_info("Starting NanoStack Border Router...");
    tr_info("Build date: %s %s", __DATE__, __TIME__);
#ifdef MBED_MAJOR_VERSION
    tr_info("Mbed OS: %d", MBED_VERSION);
#endif

#if defined(MBED_SYS_STATS_ENABLED)
    mbed_stats_sys_t stats;
    mbed_stats_sys_get(&stats);

    /* ARM = 1, GCC_ARM = 2, IAR = 3 */
    tr_info("Compiler ID: %d", stats.compiler_id);
    /* Compiler versions:
       ARM: PVVbbbb (P = Major; VV = Minor; bbbb = build number)
       GCC: VVRRPP  (VV = Version; RR = Revision; PP = Patch)
       IAR: VRRRPPP (V = Version; RRR = Revision; PPP = Patch)
    */
    tr_info("Compiler Version: %d", stats.compiler_version);
#endif
}

/**
 * \brief The entry point for this application.
 * Sets up the application and starts the border router module.
 */
int main(int argc, char **argv)
{
    ns_hal_init(app_stack_heap, APP_DEFINED_HEAP_SIZE, app_heap_error_handler, &heap_info);

    mbed_trace_init(); // set up the tracing library
    mbed_trace_print_function_set(trace_printer);
    mbed_trace_config_set(TRACE_MODE_COLOR | APP_TRACE_LEVEL | TRACE_CARRIAGE_RETURN);

    // Have to let mesh_system do net_init_core in case we use
    // Nanostack::add_ethernet_interface()
    mesh_system_init();

#if MBED_CONF_APP_BACKHAUL_MAC_SRC == BOARD
    mbed_mac_address((char *)mac);
#endif

    if (MBED_CONF_APP_LED != NC) {
        led_ticker.attach_us(toggle_led1, 500000);
    }
    border_router_tasklet_start();
}

/**
 * \brief Error handler for errors in dynamic memory handling.
 */
static void app_heap_error_handler(heap_fail_t event)
{
    tr_error("Dyn mem error %x", (int8_t)event);

    switch (event) {
        case NS_DYN_MEM_NULL_FREE:
            break;
        case NS_DYN_MEM_DOUBLE_FREE:
            break;
        case NS_DYN_MEM_ALLOCATE_SIZE_NOT_VALID:
            break;
        case NS_DYN_MEM_POINTER_NOT_VALID:
            break;
        case NS_DYN_MEM_HEAP_SECTOR_CORRUPTED:
            break;
        case NS_DYN_MEM_HEAP_SECTOR_UNITIALIZED:
            break;
        default:
            break;
    }

    while (1);
}

