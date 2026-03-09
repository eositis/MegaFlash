#ifndef _TFTPRXTASK_H
#define _TFTPRXTASK_H

#include "tftptask.h"

#define TFTP_RX_QUEUE_SIZE  16
#define TFTP_RX_QUEUE_ENTRY_SIZE  (4 + 1024)  /* opcode(2)+block(2)+data(1024) */

class CTFTPRXTask:public CTFTPTask {
public:
  CTFTPRXTask(const uint32_t unitNum,const char* hostname,const char* filename,const bool enable1kBlockSize,const uint32_t tftpTimeout,
              const uint32_t tftpMaxAttempt,const uint16_t tftpServerPort);

  //Override Run()
  virtual void Run(const char* ssid, const char* wpakey);

  bool OnUDPPacketReceived(struct pbuf *pbuf, const ip_addr_t *remote_addr, u16_t remote_port) override;
  bool DrainOneQueuedPacket() override;
  
protected:
  bool OACKReceived;          //To indicate OACK Packet has been received
  uint16_t expectedBlock;     //The TFTP block number we are expecting
  bool hasCompleted;          //To indicate the transfer has completed and the last Ack is being handled
  uint32_t blockReceived;     //Number of ProDOS block received
  uint32_t blockCapacity;     //The capacity of the unit in number of ProDOS blocks.
  uint32_t tftpBlockSize;     //TFTP block size (512 or 1024)
  bool serverTIDAccepted;     //Server TID (remote_port) accepted

  //Event Handlers
  //void EvtStart();
  void EvtDNSResult(const int dns_error, const ip_addr_t *ipaddr);
  void EvtUDPReceived(const uint8_t* payload,uint16_t payloadlen,ip_addr_t remote_addr,uint16_t remote_port);
  void EvtTimeout(uint32_t arg);
  
  void StartTransfer();  
  void Retry(); 
  void ProcessOACKPacket(const uint8_t* payload,uint16_t payloadlen,uint16_t remote_port);
  void ProcessDataPacket(const uint8_t* payload,uint16_t payloadlen,uint16_t remote_port);
  void HandleOACK_blksize(const char* value);
  void HandleOACK_tsize(const char* value);
private:
  bool IsValidBlockNumber(const uint32_t blockNum);

  /** When true, incoming DATA packets are enqueued instead of invoking EvtUDPReceived (used during blocking flash write). */
  volatile bool enqueue_data_packets;
  /** Queue of received DATA payloads (len + data) for draining after flash write. */
  struct { uint16_t len; uint8_t data[TFTP_RX_QUEUE_ENTRY_SIZE]; } rx_queue[TFTP_RX_QUEUE_SIZE];
  uint8_t rx_queue_head;
  uint8_t rx_queue_tail;
  uint8_t rx_queue_count;
};



#endif
