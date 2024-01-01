#include "i2c.h"

#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "pico/multicore.h" // <- for multicore save malloc
#include "pico/malloc.h"
#include "pico/mem_ops.h"

#include "debug_pins.h"

// Type defines
enum TransactionDirection {
    TRANSDIR_NONE = 0,
    TRANSDIR_HIDO = 1,
    TRANSDIR_HODI = 2
};

enum TransactionType {
    TRANSTYPE_NONE = 0,
    TRANSTYPE_REGISTER = 1,
    TRANSTYPE_PDO = 2,
    TRANSTYPE_ERROR = 3
};

enum TransactionPhase {
    TRANSPHASE_NONE = 0,
    TRANSPHASE_CMD_RECEIVED = 1,
    TRANSPHASE_FIRST_BYTE_SEND = 2,
    TRANSPHASE_DONE = 3
};

enum FIFO_DIRECTION {
    FIFO_DIR_TX = 0,
    FIFO_DIR_RX = 1
};


// global variables
static TransactionDirection g_transactionDir = TRANSDIR_NONE;
static TransactionType g_transactionType = TRANSTYPE_NONE;
static TransactionPhase g_transactionPhase = TRANSPHASE_NONE;
static uint32_t g_transactionAddr = 0xFF; // 0xFF = none, 0x00 - 0x3F = address

//hardware
static i2c_inst_t *g_i2c;
static uint32_t g_i2cAddr;
static uint32_t g_sdaPin;
static uint32_t g_sclPin;

// buffer related variables
static volatile uint16_t *g_HInStreamBuffer[2] = {0}; // Buffer that contains the pds data and the one byte status.
static volatile uint16_t *g_pdsData[2] = {0};
static volatile uint32_t g_pdsDataLen = 0;
static volatile uint32_t g_activePdsRxChannel = 0;
static volatile uint32_t g_activePdsTxChannel = 0;
static volatile uint32_t g_PdsChannelFull = 0;
static volatile bool g_pdsOverflow = false;
static volatile bool g_pdsUnderflow = false;
static volatile uint16_t *g_registerData[2] = {0};

//dma
static dma_channel_config g_i2cDmaConfig;
static uint32_t g_i2cDmaChan;

// callbacks
static bool (*H_Out_PDSCallback)(uint16_t *pdoData, uint32_t pdoDataLen);
static bool (*H_In_RegisterCallback)(void* buffer, uint32_t *length, uint32_t registerAddr);
static bool (*H_In_StatusCallback)(uint8_t *status);
static bool (*H_Out_RegisterCallback)(void* buffer, uint32_t length, uint32_t registerAddr);

// private prototype functions
static void __isr __not_in_flash_func(i2c_slave_irq_handler)(void);
void __isr __not_in_flash_func(i2c_dma_irq_handler)(void);

static void __always_inline H_IN_PdsData(void);
static void __always_inline H_In_RegisterTransfer(uint32_t addr);
static void __always_inline H_Out_RegisterTransfer(uint32_t addr);

void I2C_Init(i2cInitConfiguration_t *initConfiguration)
{
    // Set hardware configuration variables
    g_i2c = initConfiguration->i2c;
    g_sdaPin = initConfiguration->sdaPin;
    g_sclPin = initConfiguration->sclPin;
    g_i2cAddr = initConfiguration->i2cAddr;

    // Set callback functions
    H_Out_PDSCallback = initConfiguration->H_Out_PDSCallback;
    H_In_RegisterCallback = initConfiguration->H_In_RegisterCallback;
    H_In_StatusCallback = initConfiguration->H_In_StatusCallback;
    H_Out_RegisterCallback = initConfiguration->H_Out_RegisterCallback;

    // I2C Initialisation. Using it at 1 MHz.
    i2c_init(g_i2c, 1000*1000);
    i2c_set_slave_mode(g_i2c, true, g_i2cAddr);
    
    gpio_set_function(g_sdaPin, GPIO_FUNC_I2C);
    gpio_set_function(g_sclPin, GPIO_FUNC_I2C);
    gpio_pull_up(g_sdaPin);
    gpio_pull_up(g_sclPin);

    // Malloc memory for the data buffers
    g_pdsDataLen = initConfiguration->pdoDataLen;
    g_HInStreamBuffer[0] = (uint16_t*)malloc((g_pdsDataLen + 1) * 2);
    g_HInStreamBuffer[1] = (uint16_t*)malloc((g_pdsDataLen + 1) * 2);
    g_pdsData[0] = g_HInStreamBuffer[0]+1;
    g_pdsData[1] = g_HInStreamBuffer[1]+1;
    g_registerData[0] = (uint16_t*)malloc(initConfiguration->longestRegisterLength * 2);
    g_registerData[1] = (uint16_t*)malloc(initConfiguration->longestRegisterLength * 2);

    // init dma
    // init i2c dma
    g_i2cDmaChan = dma_claim_unused_channel(true);
    g_i2cDmaConfig = dma_channel_get_default_config(g_i2cDmaChan);
    channel_config_set_transfer_data_size(&g_i2cDmaConfig, DMA_SIZE_16);
    channel_config_set_read_increment(&g_i2cDmaConfig, true);
    channel_config_set_write_increment(&g_i2cDmaConfig, false);
    channel_config_set_dreq(&g_i2cDmaConfig, i2c_get_dreq(g_i2c, true));
    dma_channel_configure(
        g_i2cDmaChan,              // Channel to be configured
        &g_i2cDmaConfig,             // The configuration we just created
        &g_i2c->hw->data_cmd,   // The write address
        0,           // The read address
        0,              // Number of transfers; 
        false           // Don't start yet
    );
    dma_channel_set_irq0_enabled(g_i2cDmaChan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, i2c_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    
    // Only enable interrupts we want to use
    g_i2c->hw->intr_mask =
            I2C_IC_INTR_MASK_M_RX_FULL_BITS | I2C_IC_INTR_MASK_M_RD_REQ_BITS | I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS |
            I2C_IC_INTR_MASK_M_STOP_DET_BITS | I2C_IC_INTR_MASK_M_RX_DONE_BITS;

    // Enable interrupts
    irq_set_exclusive_handler(I2C0_IRQ, i2c_slave_irq_handler);
    irq_set_enabled(I2C0_IRQ, true);
}

void I2C_send_H_In_PDSData(uint8_t *data, uint32_t len)
{
    // Check if the data fits into the buffer.
    assert(len == g_pdsDataLen);
    g_activePdsRxChannel = !g_activePdsRxChannel;
    if(g_PdsChannelFull & (1 << g_activePdsRxChannel))
    {
        // Channel full -> overflow
        g_pdsOverflow = true;
    }
    
    // Transform the data from byte array to 16 bit array.
    volatile uint16_t *buffer16 = g_pdsData[g_activePdsRxChannel];
    for (uint32_t i = 0; i < len; i++)
    {
        buffer16[i] = data[i] & 0xFF;
    }

    // Write status byte to the beginnign of the buffer.
    uint16_t status = 0;
    H_In_StatusCallback((uint8_t*)&status);
    g_HInStreamBuffer[g_activePdsRxChannel][0] = status & 0xFF;

    g_PdsChannelFull |= 1 << g_activePdsRxChannel;
}

// ---- private functions ----

/**
 * @brief Prepares the tx dma to send the pdo to host.
 * 
 */
static void __always_inline H_IN_PdsData(void) {
    // Write data to the tx fifo.
    auto hw = g_i2c->hw;
    g_activePdsTxChannel = !g_activePdsTxChannel;
    if(!(g_PdsChannelFull & (1 << g_activePdsTxChannel)))
    {
        // Channel not full -> underflow
        g_pdsUnderflow = true;
    }
    dma_channel_set_trans_count(g_i2cDmaChan, g_pdsDataLen + 1, false);
    dma_channel_set_read_addr(g_i2cDmaChan, g_HInStreamBuffer[g_activePdsTxChannel], true);
}


/**
 * @brief Prepares the tx dma to send value of the register with address addr to host.
 * 
 */
static void __always_inline H_In_RegisterTransfer(uint32_t addr) {
    auto hw = g_i2c->hw;
    volatile uint16_t *regData = g_registerData[FIFO_DIR_TX];
    uint32_t regLength = 0;

    // load the data from the register into the tx fifo.
    bool valid = H_In_RegisterCallback((void*)regData, &regLength, addr);

    // Transform the data from byte array to 16 bit array.
    // Loop backwards to avoid overwriting data.
    for (int32_t i = regLength-1; i >= 0; i--)
    {
        regData[i] = ((uint8_t*) regData)[i];
    }
    
    // Setup the dma channel to send the data.
    dma_channel_set_trans_count(g_i2cDmaChan, regLength, false);
    dma_channel_set_read_addr(g_i2cDmaChan, g_registerData[FIFO_DIR_TX], true);
}

/**
 * @brief Send the status byte to host.
 * 
 */
static void __always_inline H_In_Status(void) {
    auto hw = g_i2c->hw;

    uint16_t status = 0;
    H_In_StatusCallback((uint8_t*)&status);

    hw->data_cmd = status;
}

/**
 * @brief Sets the value of the register with address addr to data.
 * 
 */
static void __always_inline H_Out_RegisterTransfer(uint32_t addr) {
    auto hw = g_i2c->hw;
    volatile uint32_t length = hw->rxflr+1;
    volatile uint8_t *regData = (uint8_t*)g_registerData[FIFO_DIR_RX];

    for (uint32_t i = 0; i < length; i++)
    {
        regData[i] = hw->data_cmd;
    }

    H_Out_RegisterCallback((void*)regData, length, addr);
}


// i2c slave handler
static void __isr __not_in_flash_func(i2c_slave_irq_handler)(void) {
    gpio_put(DEBUG_PIN3, 1);
    // Read the interrupt status register to see what caused this interrupt.
    auto hw = g_i2c->hw;
    volatile uint32_t intr_stat = hw->intr_stat;
    if (intr_stat == 0) {
        return;
    }
    

    // There was data left in the tx-fifo that is now cleared.
    if (intr_stat & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
        // Clear the abort state before attempting to write to the tx fifo again.
        volatile uint32_t tx_abrt_source = hw->tx_abrt_source;
        hw->clr_tx_abrt;
    }

    // There was a addr match and we are being asked to send data.
    if (intr_stat & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
        hw->clr_rd_req;
        
        if (g_transactionPhase == TRANSPHASE_CMD_RECEIVED && g_transactionDir != TRANSDIR_HODI)
        {
            // We did already receive the cmd, so we now what to send
            if (g_transactionType == TRANSTYPE_PDO)
            {
                H_IN_PdsData();
            }
            else if (g_transactionType == TRANSTYPE_REGISTER)
            {
                H_In_RegisterTransfer(g_transactionAddr);
            }
        }
        else
        {
            // if this is the first byte, we shall answer with a PDO.
            if(g_transactionPhase == TRANSPHASE_NONE)
            {
                g_transactionPhase = TRANSPHASE_FIRST_BYTE_SEND;
                g_transactionType = TRANSTYPE_PDO;

                H_IN_PdsData();
            }
            else
            {
                // We are not supposed to send data, so we just send 0x00
                hw->data_cmd = 0x00;
                g_transactionType = TRANSTYPE_ERROR;
            }
            
        }
    }

    // The tx fifo is empty.
    if (intr_stat & I2C_IC_INTR_STAT_R_TX_EMPTY_BITS) {
        // We have to write something into the fifo to clear the interrupt.
        
        hw->data_cmd = 0x55;
        g_transactionType = TRANSTYPE_ERROR;
    }

    if (intr_stat & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {

        hw->clr_stop_det;

                // Read is terminated, so we can process the data.
        if (g_transactionDir == TRANSDIR_HODI)
        {
            // This is HoDi, so we need to set the register.
            H_Out_RegisterTransfer(g_transactionAddr);
        }

        g_transactionAddr = 0xFF;
        g_transactionDir = TRANSDIR_NONE;
        g_transactionType = TRANSTYPE_NONE;
        g_transactionPhase = TRANSPHASE_NONE;
        hw->rx_tl = 0;
    }

    if (intr_stat & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {

        // Read data from the rx fifo - also clears the interrupt.
        volatile uint32_t rxReg = hw->data_cmd;   
        volatile uint32_t data = rxReg & 0xFF;

        // If this is the first byte, it may contain the cmd.
        if (rxReg & (1 << 11) && g_transactionPhase == TRANSPHASE_NONE)
        {  
            g_transactionPhase = TRANSPHASE_CMD_RECEIVED;       
            if (data & 0x80)
            {
                // this is a pdo transaction.
                g_transactionAddr = 0xFF;
                g_transactionType = TRANSTYPE_PDO;

                bool HiDo = data & 0x40;
                g_transactionDir = HiDo? TRANSDIR_HIDO : TRANSDIR_HODI;
                
            }
            else
            {
                // if the msb is cleared, this is a register access.
                g_transactionAddr = data & 0x3F;
                g_transactionType = TRANSTYPE_REGISTER;

                // This is a cmd byte.
                if (data & 0x40)
                {
                    // This is HiDo.
                    g_transactionDir = TRANSDIR_HIDO;
                }
                else
                {
                    // This is HoDi.
                    g_transactionDir = TRANSDIR_HODI;

                    hw->rx_tl = 15;
                }
            }
        }
    }

    if (intr_stat & I2C_IC_INTR_STAT_R_RX_DONE_BITS) {
        hw->clr_rx_done;
        
    }

    // There shouldn't be any interrupts that were there at IRQ entry we didn't handle.
    if ((hw->intr_stat & intr_stat) != 0) {
        volatile uint32_t intr_stat_dbg = hw->intr_stat;
        volatile uint32_t tx_abrt_source = hw->tx_abrt_source;
        __breakpoint();
    }

    gpio_put(DEBUG_PIN3, 0);
}


void __isr __not_in_flash_func(i2c_dma_irq_handler)(void)
{
    if(dma_channel_get_irq0_status(g_i2cDmaChan))
    {
        dma_channel_acknowledge_irq0(g_i2cDmaChan);
        // The pdo transfer is done.
        g_PdsChannelFull &= ~(1 << g_activePdsTxChannel);
    }
}