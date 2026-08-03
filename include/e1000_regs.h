#ifndef E1000_REGS_H
#define E1000_REGS_H

#include <exec/types.h>

/* Ring sizing constants */
#define V1000_RING_ENTRIES  16
#define V1000_DESC_SIZE     16
#define V1000_RX_BUFSIZE    2048

/* e1000 register offsets (Intel 82540EM) */
#define E1000_REG_CTRL     0x00000
#define E1000_REG_STATUS   0x00008
#define E1000_REG_ICR      0x000C0   /* Interrupt Cause Read (RO/read-to-clear) */
#define E1000_REG_ICS      0x000C8   /* Interrupt Cause Set (WO, forces IRQ) */
#define E1000_REG_IMS      0x000D0   /* Interrupt Mask Set */
#define E1000_REG_IMC      0x000D8   /* Interrupt Mask Clear (WO, write 1 to mask) */
#define E1000_REG_RCTL     0x00100   /* Receive Control */
#define E1000_REG_TCTL     0x00400   /* Transmit Control */
#define E1000_REG_RDBAL    0x02800   /* RX Descriptor Base Address Low */
#define E1000_REG_RDBAH    0x02804   /* RX Descriptor Base Address High */
#define E1000_REG_RDLEN    0x02808   /* RX Descriptor Length (in bytes) */
#define E1000_REG_RDH      0x02810   /* RX Descriptor Head */
#define E1000_REG_RDT      0x02818   /* RX Descriptor Tail */
#define E1000_REG_TDBAL    0x03800   /* TX Descriptor Base Address Low */
#define E1000_REG_TDBAH    0x03804
#define E1000_REG_TDLEN    0x03808
#define E1000_REG_TDH      0x03810
#define E1000_REG_TDT      0x03818
#define E1000_REG_TPT      0x040D4   /* Total Packets Transmitted */
#define E1000_REG_RAL      0x05400   /* Receive Address Low  */
#define E1000_REG_RAH      0x05404   /* Receive Address High */

/* Control/Status bits */
#define E1000_CTRL_RST            (1U << 26)  /* Device Reset */
#define E1000_STATUS_LU           (1U << 1)   /* Link Up */
#define E1000_STATUS_FD           (1U << 0)   /* Full Duplex */
#define E1000_STATUS_SPEED_MASK   (3U << 6)
#define E1000_STATUS_SPEED_SHIFT  6

/* RCTL bits */
#define E1000_RCTL_EN             (1U << 1)   /* Receiver Enable */
#define E1000_RCTL_BAM            (1U << 15)  /* Broadcast Accept Mode */
#define E1000_RCTL_BSIZE_2048     (0U << 16)  /* Buffer Size 2048 bytes */

/* TCTL bits */
#define E1000_TCTL_EN             (1U << 1)   /* Transmit Enable */
#define E1000_TCTL_PSP            (1U << 3)   /* Pad Short Packets */
#define E1000_TCTL_CT(v)          (((v) & 0xff)  << 4)
#define E1000_TCTL_COLD(v)        (((v) & 0x3ff) << 12)

/* Interrupt Cause bits */
#define E1000_ICR_TXDW            (1U << 0)   /* Transmit Descriptor Written Back */
#define E1000_ICR_TXQE            (1U << 1)   /* Transmit Queue Empty */
#define E1000_ICR_LSC             (1U << 2)   /* Link Status Change */
#define E1000_ICR_RXSEQ           (1U << 3)   /* Receive Sequence Error */
#define E1000_ICR_RXDMT0          (1U << 4)   /* Receive Descriptor Min Threshold */
#define E1000_ICR_RXO             (1U << 6)   /* Receiver Overrun */
#define E1000_ICR_RXT0            (1U << 7)   /* Receiver Timer Interrupt (Packet arrived) */

/* Legacy TX descriptor CMD bits (byte 11 of the 16-byte descriptor). */
#define E1000_TXD_CMD_EOP         (1U << 0)   /* End Of Packet */
#define E1000_TXD_CMD_IFCS        (1U << 1)   /* Insert FCS (CRC) */
#define E1000_TXD_CMD_RS          (1U << 3)   /* Report Status — set STATUS.DD */

/* Legacy TX descriptor STATUS bits (byte 12). */
#define E1000_TXD_STAT_DD         (1U << 0)   /* Descriptor Done */

static inline void poke_le32(volatile void *p, uint32 v)
{
    *(volatile uint32 *)p = __builtin_bswap32(v);
}

static inline uint32 peek_le32(volatile void *p)
{
    return __builtin_bswap32(*(volatile uint32 *)p);
}

static inline void v1000_wmb(void)
{
    __asm__ volatile ("sync" : : : "memory");
}

static inline uint32 e1000_read32(volatile void *base, uint32 off)
{
    volatile uint32 *p = (volatile uint32 *)((volatile uint8 *)base + off);
    uint32 v = __builtin_bswap32(*p);
    __asm__ volatile ("eieio" : : : "memory");
    return v;
}

static inline void e1000_write32(volatile void *base, uint32 off, uint32 val)
{
    volatile uint32 *p = (volatile uint32 *)((volatile uint8 *)base + off);
    *p = __builtin_bswap32(val);
    __asm__ volatile ("eieio" : : : "memory");
}

#endif /* E1000_REGS_H */
