#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cache.h"
#include "jbod.h"
#include "mdadm.h"
#include "net.h"

int is_mounted = 0; // mount status
int is_written = 0; // write permission

int mdadm_mount(void) {
  if (is_mounted == 1) { // already mounted
    return -1;
  }
  else {
    uint32_t op = JBOD_MOUNT << 12; // cmd = 0b0; rest are 0;
    if (jbod_client_operation(op, NULL) == JBOD_NO_ERROR) {
      is_mounted = 1; // mounted
      return 1;
    } else {
      return -1;
    }
  }
}

int mdadm_unmount(void) {
  if (is_mounted == 0) { // already unmounted
    return -1;
  }
  else {
    uint32_t op = JBOD_UNMOUNT << 12; // cmd = 0b1; rest are 0;
    if (jbod_client_operation(op, NULL) == JBOD_NO_ERROR) {
      is_mounted = 0; // unmounted
      return 1;
    } else {
      return -1;
    }
  }
}

int mdadm_write_permission(void) {
  if (is_written == 1) { // write permission already allowed
    return -1;
  } else {
    uint32_t op = JBOD_WRITE_PERMISSION << 12;
    if (jbod_client_operation(op, NULL) == JBOD_NO_ERROR) {
      is_written = 1; // permission granted
      return 0;
    } else {
      return -1; // failed
    }
  }
}


int mdadm_revoke_write_permission(void){
  if (is_written == 0) { // write permission already revoked
    return -1;
  } else {
    uint32_t op = JBOD_REVOKE_WRITE_PERMISSION << 12; 
    if (jbod_client_operation(op, NULL) == JBOD_NO_ERROR) {
      is_written = 0; // permission revoked
      return 0;
    } else {
      return -1; // failed
    }
  }
}

/*
Given an address, retrieve the id of corresponding disk and block
Successful: return 0
Invalid Disk: return 7
Invalid Block: return 8
*/
int get_loc(uint32_t addr, uint32_t *disk_id, uint32_t *block_id) {
  int d_id = addr / JBOD_DISK_SIZE; // disk index
  int b_id = (addr % JBOD_DISK_SIZE) / JBOD_BLOCK_SIZE; // block index
  *disk_id = d_id;
  *block_id = b_id;
  if (0 <= d_id && d_id < JBOD_NUM_DISKS) { // valid disk
    if (0 <= b_id && b_id < JBOD_NUM_BLOCKS_PER_DISK) { // valid block
      return JBOD_NO_ERROR;
    } else {
      return JBOD_BAD_BLOCK_NUM;
    }
  }
  return JBOD_BAD_DISK_NUM;
}

int read_precheck(uint32_t start_addr, uint32_t read_len, uint8_t *read_buf) {
  if (is_mounted == 0) return -1; // not mounted
  if (read_len > 2048) return -1; // write more than 2,048 bytes
  if (read_buf == NULL) { // buffer is null
    return (read_len == 0) ? 0 : -1; // fail if len != 0 / success otherwise
  }
  uint32_t ldisk_id, lblock_id; // memory location of last pointed address
  if (start_addr + read_len <= (JBOD_DISK_SIZE * JBOD_NUM_DISKS)) return 0; // over the linear space
  if (get_loc(start_addr + read_len, &ldisk_id, &lblock_id) != JBOD_NO_ERROR) return -1; // invalid address
  return 0;
}



int mdadm_read(uint32_t addr, uint32_t len, uint8_t *buf) {
  if (read_precheck(addr, len, buf) == JBOD_NO_ERROR) { // parameter validation
    if (len == 0) {
      buf = NULL;
      return 0;
    }
    uint32_t sdisk_id, sblock_id, srem_bytes; // disk & block id of first addr
    get_loc(addr, &sdisk_id, &sblock_id);
    uint32_t ldisk_id, lblock_id, lrem_bytes; // disk & block id of last addr
    get_loc(addr + len - 1, &ldisk_id, &lblock_id);
    srem_bytes = (addr % JBOD_DISK_SIZE) % JBOD_BLOCK_SIZE; // index of start address in the block
    lrem_bytes = ((addr + len - 1) % JBOD_DISK_SIZE) % JBOD_BLOCK_SIZE; // index of last address in the block
    uint32_t current_disk, current_block;
	  get_loc(addr, &current_disk, &current_block);
    uint16_t offset = 0;
	  while (current_disk <= ldisk_id) {
      uint32_t op = (JBOD_SEEK_TO_DISK << 12) | (current_disk << 8);
      jbod_client_operation(op, NULL);
      while (current_block <= ((current_disk == ldisk_id) ? lblock_id : JBOD_NUM_BLOCKS_PER_DISK-1)) {
        uint8_t temp_buf[JBOD_BLOCK_SIZE];
        if (cache_enabled()) {
          if (cache_lookup(current_disk, current_block, temp_buf) == 1) {
            // found in cache -> directly return from cache
            int start_cpy = ((current_disk == sdisk_id) && (current_block == sblock_id)) ? srem_bytes : 0;
            int end_cpy = ((current_disk == ldisk_id) && (current_block == lblock_id)) ? lrem_bytes : JBOD_BLOCK_SIZE - 1;
            memcpy(buf+offset, &temp_buf[start_cpy], (end_cpy-start_cpy+1));
            offset += end_cpy-start_cpy+1;
            ++current_block;
            continue;
          }
          else {
            // not found in cache -> go to jbod and fetch to cache & return it
            uint32_t op = (JBOD_SEEK_TO_BLOCK << 12) | current_block;
            jbod_client_operation(op, NULL);
            op = (JBOD_READ_BLOCK << 12);
            jbod_client_operation(op, temp_buf);
            // insert to cache
            cache_insert(current_disk, current_block, temp_buf);
          }
        } else {
          // cache not available -> go to jbod and return
          uint32_t op = (JBOD_SEEK_TO_BLOCK << 12) | current_block;
          jbod_client_operation(op, NULL);
          op = (JBOD_READ_BLOCK << 12);
          jbod_client_operation(op, temp_buf);
        }
        // select appropriate portion of the block
        int start_cpy = ((current_disk == sdisk_id) && (current_block == sblock_id)) ? srem_bytes : 0;
        int end_cpy = ((current_disk == ldisk_id) && (current_block == lblock_id)) ? lrem_bytes : JBOD_BLOCK_SIZE - 1;
        memcpy(buf+offset, &temp_buf[start_cpy], (end_cpy-start_cpy+1));
        offset += end_cpy-start_cpy+1;
        ++current_block;
      }
      current_block = 0; // reset to point first block in the disk
      ++current_disk; // go to the next disk
    }
    return offset; // success
  }
  else {
    return -1;
  }
  return 0;
}

int write_precheck(uint32_t start_addr, uint32_t write_len, const uint8_t *write_buf) {
  if (is_mounted == 0) return -1; // not mounted
  if (is_written == 0) return -1; // no write permission
  if (write_len > 2048) return -1; // write more than 2,048 bytes
  if (write_buf == NULL) { // buffer is null
    return (write_len == 0) ? 0 : -1; // fail if len != 0 / success otherwise
  }
  if (start_addr + write_len <= (JBOD_DISK_SIZE * JBOD_NUM_DISKS)) return 0; // over the linear space
  uint32_t ldisk_id, lblock_id; // memory location of last pointed address
  if (get_loc(start_addr + write_len, &ldisk_id, &lblock_id) != JBOD_NO_ERROR) return -1; // invalid address
  return 0;
}


int mdadm_write(uint32_t addr, uint32_t len, const uint8_t *buf) {
  if (write_precheck(addr, len, buf) == JBOD_NO_ERROR) { // parameter validation
    if (len == 0) return 0;
    uint32_t sdisk_id, sblock_id, srem_bytes; // start address
    get_loc(addr, &sdisk_id, &sblock_id);
    uint32_t ldisk_id, lblock_id, lrem_bytes; // last address
    get_loc(addr + len - 1, &ldisk_id, &lblock_id);
    // index of an addr within the block
    srem_bytes = (addr % JBOD_DISK_SIZE) % JBOD_BLOCK_SIZE;
    lrem_bytes = ((addr + len - 1) % JBOD_DISK_SIZE) % JBOD_BLOCK_SIZE;
    uint32_t current_disk, current_block;
	  get_loc(addr, &current_disk, &current_block);
    uint16_t offset = 0;
    while (current_disk <= ldisk_id) {
      uint32_t op = (JBOD_SEEK_TO_DISK << 12) | (current_disk << 8);
      jbod_client_operation(op, NULL);
      while (current_block <= ((current_disk == ldisk_id) ? lblock_id : JBOD_NUM_BLOCKS_PER_DISK-1)) {
        op = (JBOD_SEEK_TO_BLOCK << 12) | current_block;
        jbod_client_operation(op, NULL);
        uint8_t temp_buf[JBOD_BLOCK_SIZE];
        op = (JBOD_READ_BLOCK << 12);
        jbod_client_operation(op, temp_buf);
        // construct temporary buffer before writing
        int start_backup = ((current_disk == sdisk_id) && (current_block == sblock_id)) ? srem_bytes : 0;
        int end_backup = ((current_disk == ldisk_id) && (current_block == lblock_id)) ? lrem_bytes : JBOD_BLOCK_SIZE - 1;
        memcpy(&temp_buf[start_backup], buf+offset, (end_backup-start_backup+1));
        op = (JBOD_SEEK_TO_DISK << 12) | (current_disk << 8);
        jbod_client_operation(op, NULL);
        op = (JBOD_SEEK_TO_BLOCK << 12) | current_block;
        jbod_client_operation(op, NULL);
        op = (JBOD_WRITE_BLOCK << 12);
        jbod_client_operation(op, temp_buf);
        // write through cache
        if (cache_enabled()) {
          if (cache_lookup(current_disk, current_block, temp_buf) == 1) {
            // if it already exists
            cache_update(current_disk, current_block, temp_buf);
          } else {
            // if it does not exist
            cache_insert(current_disk, current_block, temp_buf);
          }
        }
        offset += end_backup-start_backup+1;
        ++current_block;
      }
      current_block = 0;
      ++current_disk;
    }
    return offset; // success
  }
  else {
    return -1; // invalid parameter
  }
  return 0;
}
