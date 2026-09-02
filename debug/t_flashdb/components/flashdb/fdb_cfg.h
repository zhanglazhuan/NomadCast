/*
 * FlashDB configuration for t_flashdb — KVDB only, file storage mode on SD/FAT.
 * (Renamed from vendor/FlashDB/inc/fdb_cfg_template.h; flashdb.h includes <fdb_cfg.h>.)
 */
#ifndef _FDB_CFG_H_
#define _FDB_CFG_H_

/* using KVDB feature */
#define FDB_USING_KVDB

/* File storage mode via POSIX API (open/lseek/read/write/close).
 * The DB is stored as files on the mounted FAT/SD filesystem — no flash partition. */
#define FDB_USING_FILE_POSIX_MODE

/* Verbose logs during bring-up; comment out for the performance runs. */
#define FDB_DEBUG_ENABLE

#endif /* _FDB_CFG_H_ */
