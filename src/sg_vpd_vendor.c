/*
 * Copyright (c) 2006-2023 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#ifndef SG_LIB_MINGW
#include <time.h>
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "sg_lib.h"
#include "sg_cmds_basic.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"

#include "sg_vpd_common.h"

/* This is a companion file to sg_vpd.c . It contains logic to output and
   decode vendor specific VPD pages

   This program fetches Vital Product Data (VPD) pages from the given
   device and outputs it as directed. VPD pages are obtained via a
   SCSI INQUIRY command. Most of the data in this program is obtained
   from the SCSI SPC-4 document at https://www.t10.org .

   Acknowledgments:
      - Lars Marowsky-Bree <lmb at suse dot de> contributed Unit Path Report
        VPD page decoding for EMC CLARiiON devices [20041016]
      - Hannes Reinecke <hare at suse dot de> contributed RDAC vendor
        specific VPD pages [20060421]
      - Jonathan McDowell <noodles at hp dot com> contributed HP/3PAR InServ
        VPD page [0xc0] containing volume information [20110922]

*/

/* vendor/product identifiers */
#define VPD_VP_SEAGATE 0
#define VPD_VP_RDAC 1
#define VPD_VP_EMC 2
#define VPD_VP_DDS 3
#define VPD_VP_HP3PAR 4
#define VPD_VP_IBM_LTO 5
#define VPD_VP_HP_LTO 6
#define VPD_VP_WDC_HITACHI 7
#define VPD_VP_NVME 8
#define VPD_VP_SG 9     /* this package/library as a vendor */


/* vendor VPD pages */
#define VPD_V_HIT_PG3 0x3
#define VPD_V_HP3PAR 0xc0
#define VPD_V_FIRM_SEA  0xc0
#define VPD_V_UPR_EMC  0xc0
#define VPD_V_HVER_RDAC  0xc0
#define VPD_V_FVER_DDS 0xc0
#define VPD_V_FVER_LTO 0xc0
#define VPD_V_DCRL_LTO 0xc0
#define VPD_V_DATC_SEA  0xc1
#define VPD_V_FVER_RDAC  0xc1
#define VPD_V_HVER_LTO 0xc1
#define VPD_V_DSN_LTO 0xc1
#define VPD_V_JUMP_SEA 0xc2
#define VPD_V_SVER_RDAC 0xc2
#define VPD_V_PCA_LTO 0xc2
#define VPD_V_DEV_BEH_SEA 0xc3
#define VPD_V_FEAT_RDAC 0xc3
#define VPD_V_MECH_LTO 0xc3
#define VPD_V_SUBS_RDAC 0xc4
#define VPD_V_HEAD_LTO 0xc4
#define VPD_V_ACI_LTO 0xc5
#define VPD_V_DUCD_LTO 0xc7
#define VPD_V_EDID_RDAC 0xc8
#define VPD_V_MPDS_LTO 0xc8
#define VPD_V_VAC_RDAC 0xc9
#define VPD_V_RVSI_RDAC 0xca
#define VPD_V_SAID_RDAC 0xd0
#define VPD_V_HIT_PG_D1 0xd1
#define VPD_V_HIT_PG_D2 0xd2


#define DEF_ALLOC_LEN 252
#define MX_ALLOC_LEN (0xc000 + 0x80)

static bool
is_like_pdt(int actual_pdt, const struct svpd_values_name_t * vnp)
{
    if (actual_pdt == vnp->pdt)
        return true;
    if (PDT_DISK == vnp->pdt) {
        switch (actual_pdt) {
        case PDT_DISK:
        case PDT_RBC:
        case PDT_PROCESSOR:
        case PDT_SAC:
        case PDT_ZBC:
            return true;
        default:
            return false;
        }
    } else if (PDT_TAPE == vnp->pdt) {
        switch (actual_pdt) {
        case PDT_TAPE:
        case PDT_MCHANGER:
        case PDT_ADC:
            return true;
        default:
            return false;
        }
    } else
        return false;
}

static const struct svpd_values_name_t *
svpd_get_v_detail(int page_num, int vend_prod_num, int pdt)
{
    const struct svpd_values_name_t * vnp;
    int vp, ty;

    vp = (vend_prod_num < 0) ? 1 : 0;
    ty = (pdt < 0) ? 1 : 0;
    for (vnp = vendor_vpd_pg; vnp->acron; ++vnp) {
        if ((page_num == vnp->value) &&
            (vp || (vend_prod_num == vnp->subvalue)) &&
            (ty || is_like_pdt(pdt, vnp)))
            return vnp;
    }
#if 0
    if (! ty)
        return svpd_get_v_detail(page_num, vend_prod_num, -1);
    if (! vp)
        return svpd_get_v_detail(page_num, -1, pdt);
#endif
    return NULL;
}

const struct svpd_values_name_t *
svpd_find_vendor_by_num(int page_num, int vend_prod_num)
{
    const struct svpd_values_name_t * vnp;

    for (vnp = vendor_vpd_pg; vnp->acron; ++vnp) {
        if ((page_num == vnp->value) &&
            ((vend_prod_num < 0) || (vend_prod_num == vnp->subvalue)))
            return vnp;
    }
    return NULL;
}

const struct svpd_values_name_t *
svpd_find_vendor_by_acron(const char * ap)
{
    const struct svpd_values_name_t * vnp;

    for (vnp = vendor_vpd_pg; vnp->acron; ++vnp) {
        if (0 == strcmp(vnp->acron, ap))
            return vnp;
    }
    return NULL;
}

int
svpd_count_vendor_vpds(int vpd_pn, int vend_prod_num)
{
    const struct svpd_values_name_t * vnp;
    int matches;

    for (vnp = vendor_vpd_pg, matches = 0; vnp->acron; ++vnp) {
        if ((vpd_pn == vnp->value) && vnp->name) {
            if ((vend_prod_num < 0) || (vend_prod_num == vnp->subvalue)) {
                if (0 == matches)
                    printf("Matching vendor specific VPD pages:\n");
                ++matches;
                printf("  %-10s 0x%02x,%d      %s\n", vnp->acron,
                       vnp->value, vnp->subvalue, vnp->name);
            }
        }
    }
    return matches;
}

static void
dStrRaw(const uint8_t * str, int len)
{
    int k;

    for (k = 0; k < len; ++k)
        printf("%c", str[k]);
}

static void
decode_vpd_c0_hp3par(uint8_t * buff, int len)
{
    int rev;
    long offset;

    if (len < 24) {
        pr2serr("HP/3PAR vendor specific VPD page length too short=%d\n",
                len);
        return;
    }

    rev = buff[4];
    printf("  Page revision: %d\n", rev);

    printf("  Volume type: %s\n", (buff[5] & 0x01) ? "tpvv" :
            (buff[5] & 0x02) ? "snap" : "base");
    printf("  Reclaim supported: %s\n", (buff[5] & 0x04) ? "yes" : "no");
    printf("  ATS supported: %s\n", (buff[5] & 0x10) ? "yes" : "no");
    printf("  XCopy supported: %s\n", (buff[5] & 0x20) ? "yes" : "no");

    if (rev > 3) {
        printf("  VV ID: %" PRIu64 "\n", sg_get_unaligned_be64(buff + 28));
        offset = 44;
        printf("  Volume name: %s\n", &buff[offset]);

        printf("  Domain ID: %d\n", sg_get_unaligned_be32(buff + 36));

        offset += sg_get_unaligned_be32(buff + offset - 4) + 4;
        printf("  Domain Name: %s\n", &buff[offset]);

        offset += sg_get_unaligned_be32(buff + offset - 4) + 4;
        printf("  User CPG: %s\n", &buff[offset]);

        offset += sg_get_unaligned_be32(buff + offset - 4) + 4;
        printf("  Snap CPG: %s\n", &buff[offset]);

        offset += sg_get_unaligned_be32(buff + offset - 4);

        printf("  VV policies: %s,%s,%s,%s\n",
                (buff[offset + 3] & 0x01) ? "stale_ss" : "no_stale_ss",
                (buff[offset + 3] & 0x02) ? "one_host" : "no_one_host",
                (buff[offset + 3] & 0x04) ? "tp_bzero" : "no_tp_bzero",
                (buff[offset + 3] & 0x08) ? "zero_detect" : "no_zero_detect");

    }

    if (buff[5] & 0x04) {
        printf("  Allocation unit: %d\n", sg_get_unaligned_be32(buff + 8));

        printf("  Data pool size: %" PRIu64 "\n",
               sg_get_unaligned_be64(buff + 12));

        printf("  Space allocated: %" PRIu64 "\n",
               sg_get_unaligned_be64(buff + 20));
    }
    return;
}


static void
decode_firm_vpd_c0_sea(uint8_t * buff, int len)
{
    if (len < 28) {
        pr2serr("Seagate firmware numbers VPD page length too short=%d\n",
                len);
        return;
    }
    if (28 == len) {
        printf("  SCSI firmware release number: %.8s\n", buff + 4);
        printf("  Servo ROM release number: %.8s\n", buff + 20);
    } else {
        printf("  SCSI firmware release number: %.8s\n", buff + 4);
        printf("  Servo ROM release number: %.8s\n", buff + 12);
        printf("  SAP block point numbers (major/minor): %.8s\n", buff + 20);
        if (len < 36)
            return;
        printf("  Servo firmware release date: %.4s\n", buff + 28);
        printf("  Servo ROM release date: %.4s\n", buff + 32);
        if (len < 44)
            return;
        printf("  SAP firmware release number: %.8s\n", buff + 36);
        if (len < 52)
            return;
        printf("  SAP firmware release date: %.4s\n", buff + 44);
        printf("  SAP firmware release year: %.4s\n", buff + 48);
        if (len < 60)
            return;
        printf("  SAP manufacturing key: %.4s\n", buff + 52);
        printf("  Servo firmware product family and product family "
               "member: %.4s\n", buff + 56);
    }
}

static void
decode_date_code_vpd_c1_sea(uint8_t * buff, int len)
{
    if (len < 20) {
        pr2serr("Seagate Data code VPD page length too short=%d\n",
                len);
        return;
    }
    printf("  ETF log (mmddyyyy): %.8s\n", buff + 4);
    printf("  Compile date code (mmddyyyy): %.8s\n", buff + 12);
}

static void
decode_dev_beh_vpd_c3_sea(uint8_t * buff, int len)
{
    if (len < 25) {
        pr2serr("Seagate Device behaviour VPD page length too short=%d\n",
                len);
        return;
    }
    printf("  Version number: %d\n", buff[4]);
    printf("  Behaviour code: %d\n", buff[5]);
    printf("  Behaviour code version number: %d\n", buff[6]);
    printf("  ASCII family number: %.16s\n", buff + 7);
    printf("  Number of interleaves: %d\n", buff[23]);
    printf("  Default number of cache segments: %d\n", buff[24]);
}

static void
decode_rdac_vpd_c0(uint8_t * buff, int len)
{
    int memsize;
    char name[65];

    if (len < 3) {
        pr2serr("Hardware Version VPD page length too short=%d\n", len);
        return;
    }
    if (buff[4] != 'h' && buff[5] != 'w' && buff[6] != 'r') {
        pr2serr("Invalid page identifier %c%c%c%c, decoding not possible.\n",
                buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    printf("  Number of channels: %x\n", buff[8]);
    memsize = sg_get_unaligned_be16(buff + 10);
    printf("  Processor Memory Size: %d\n", memsize);
    memset(name, 0, 65);
    memcpy(name, buff + 16, 64);
    printf("  Board Name: %s\n", name);
    memset(name, 0, 65);
    memcpy(name, buff + 80, 16);
    printf("  Board Part Number: %s\n", name);
    memset(name, 0, 65);
    memcpy(name, buff + 96, 12);
    printf("  Schematic Number: %s\n", name);
    memset(name, 0, 65);
    memcpy(name, buff + 108, 4);
    printf("  Schematic Revision Number: %s\n", name);
    memset(name, 0, 65);
    memcpy(name, buff + 112, 16);
    printf("  Board Serial Number: %s\n", name);
    memset(name, 0, 65);
    memcpy(name, buff + 144, 8);
    printf("  Date of Manufacture: %s\n", name);
    memset(name, 0, 65);
    memcpy(name, buff + 152, 2);
    printf("  Board Revision: %s\n", name);
    memset(name, 0, 65);
    memcpy(name, buff + 154, 4);
    printf("  Board Identifier: %s\n", name);

    return;
}

static void
decode_rdac_vpd_c1(uint8_t * buff, int len)
{
    int i, n, v, r, m, p, d, y, num_part;
    char part[5];

    if (len < 3) {
        pr2serr("Firmware Version VPD page length too short=%d\n", len);
        return;
    }
    if (buff[4] != 'f' && buff[5] != 'w' && buff[6] != 'r') {
        pr2serr("Invalid page identifier %c%c%c%c, decoding not possible.\n",
                buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    printf("  Firmware Version: %02x.%02x.%02x\n", buff[8], buff[9], buff[10]);
    printf("  Firmware Date: %02d/%02d/%02d\n", buff[11], buff[12], buff[13]);

    num_part = (len - 12) / 16;
    n = 16;
    printf("  Partitions: %d\n", num_part);
    for (i = 0; i < num_part; i++) {
        memset(part,0, 5);
        memcpy(part, &buff[n], 4);
        printf("    Name: %s\n", part);
        n += 4;
        v = buff[n++];
        r = buff[n++];
        m = buff[n++];
        p = buff[n++];
        printf("    Version: %d.%d.%d.%d\n", v, r, m, p);
        m = buff[n++];
        d = buff[n++];
        y = buff[n++];
        printf("    Date: %d/%d/%d\n", m, d, y);

        n += 5;
    }

    return;
}

static void
decode_rdac_vpd_c3(uint8_t * buff, int len)
{
    if (len < 0x2c) {
        pr2serr("Feature parameters VPD page length too short=%d\n", len);
        return;
    }
    if (buff[4] != 'p' && buff[5] != 'r' && buff[6] != 'm') {
        pr2serr("Invalid page identifier %c%c%c%c, decoding not possible.\n",
                buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    printf("  Maximum number of drives per LUN: %d\n", buff[8]);
    printf("  Maximum number of hot spare drives: %d\n", buff[9]);
    printf("  UTM: %s\n", buff[11] & 0x80?"enabled":"disabled");
    if ((buff[11] & 0x80))
        printf("    UTM LUN: %02x\n", buff[11] & 0x7f);
    printf("  Persistent Reservations Bus Reset Support: %s\n",
           (buff[12] & 0x01) ? "enabled" : "disabled");
    return;
}

static void
decode_rdac_vpd_c4(uint8_t * buff, int len)
{
    char subsystem_id[17];
    char subsystem_rev[5];
    char slot_id[3];

    if (len < 0x1c) {
        pr2serr("Subsystem identifier VPD page length too short=%d\n", len);
        return;
    }
    if (buff[4] != 's' && buff[5] != 'u' && buff[6] != 'b') {
        pr2serr("Invalid page identifier %c%c%c%c, decoding not possible.\n",
                buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    memset(subsystem_id, 0, 17);
    memcpy(subsystem_id, &buff[8], 16);
    memset(subsystem_rev, 0, 5);
    memcpy(subsystem_rev, &buff[24], 4);
    slot_id[0] = buff[28];
    slot_id[1] = buff[29];
    slot_id[2] = 0;

    printf("  Subsystem ID: %s\n  Subsystem Revision: %s",
           subsystem_id, subsystem_rev);
    if (!strcmp(subsystem_rev, "10.0"))
        printf(" (Board ID 4884)\n");
    else if (!strcmp(subsystem_rev, "12.0"))
        printf(" (Board ID 5884)\n");
    else if (!strcmp(subsystem_rev, "13.0"))
        printf(" (Board ID 2882)\n");
    else if (!strcmp(subsystem_rev, "13.1"))
        printf(" (Board ID 2880)\n");
    else if (!strcmp(subsystem_rev, "14.0"))
        printf(" (Board ID 2822)\n");
    else if (!strcmp(subsystem_rev, "15.0"))
        printf(" (Board ID 6091)\n");
    else if (!strcmp(subsystem_rev, "16.0"))
        printf(" (Board ID 3992)\n");
    else if (!strcmp(subsystem_rev, "16.1"))
        printf(" (Board ID 3991)\n");
    else if (!strcmp(subsystem_rev, "17.0"))
        printf(" (Board ID 1331)\n");
    else if (!strcmp(subsystem_rev, "17.1"))
        printf(" (Board ID 1332)\n");
    else if (!strcmp(subsystem_rev, "17.3"))
        printf(" (Board ID 1532)\n");
    else if (!strcmp(subsystem_rev, "17.4"))
        printf(" (Board ID 1932)\n");
    else if (!strcmp(subsystem_rev, "42.0"))
        printf(" (Board ID 26x0)\n");
    else if (!strcmp(subsystem_rev, "43.0"))
        printf(" (Board ID 498x)\n");
    else if (!strcmp(subsystem_rev, "44.0"))
        printf(" (Board ID 548x)\n");
    else if (!strcmp(subsystem_rev, "45.0"))
        printf(" (Board ID 5501)\n");
    else if (!strcmp(subsystem_rev, "46.0"))
        printf(" (Board ID 2701)\n");
    else if (!strcmp(subsystem_rev, "47.0"))
        printf(" (Board ID 5601)\n");
    else
        printf(" (Board ID unknown)\n");

    printf("  Slot ID: %s\n", slot_id);

    return;
}

static void
convert_binary_to_ascii(uint8_t * src, uint8_t * dst,  int len)
{
    int i;

    for (i = 0; i < len; i++) {
        sprintf((char *)(dst+2*i), "%02x", *(src+i));
    }
}

static void
decode_rdac_vpd_c8(uint8_t * buff, int len)
{
    int i;
#ifndef SG_LIB_MINGW
    time_t tstamp;
#endif
    char *c;
    char label[61];
    int label_len;
    char uuid[33];
    int uuid_len;
    uint8_t port_id[128];
    int n;

    if (len < 0xab) {
        pr2serr("Extended Device Identification VPD page length too "
                "short=%d\n", len);
        return;
    }
    if (buff[4] != 'e' && buff[5] != 'd' && buff[6] != 'i') {
        pr2serr("Invalid page identifier %c%c%c%c, decoding not possible.\n",
                buff[4], buff[5], buff[6], buff[7]);
        return;
    }

    uuid_len = buff[11];

    for (i = 0, c = uuid; i < uuid_len; i++) {
        sprintf(c,"%02x",buff[12 + i]);
        c += 2;
    }

    printf("  Volume Unique Identifier: %s\n", uuid);
#ifndef SG_LIB_MINGW
    tstamp = sg_get_unaligned_be32(buff + 24);
    printf("    Creation Number: %d, Timestamp: %s",
           sg_get_unaligned_be16(buff + 22), ctime(&tstamp));
#else
    printf("    Creation Number: %d, Timestamp value: %u",
           sg_get_unaligned_be16(buff + 22),
           sg_get_unaligned_be32(buff + 24));
#endif
    memset(label, 0, 61);
    label_len = buff[28];
    for(i = 0; i < (label_len - 1); ++i)
        *(label + i) = buff[29 + (2 * i) + 1];
    printf("  Volume User Label: %s\n", label);

    uuid_len = buff[89];

    for (i = 0, c = uuid; i < uuid_len; i++) {
        sprintf(c,"%02x",buff[90 + i]);
        c += 2;
    }

    printf("  Storage Array Unique Identifier: %s\n", uuid);
    memset(label, 0, 61);
    label_len = buff[106];
    for(i = 0; i < (label_len - 1); ++i)
        *(label + i) = buff[107 + (2 * i) + 1];
    printf("  Storage Array User Label: %s\n", label);

    for (i = 0, c = uuid; i < 8; i++) {
        sprintf(c,"%02x",buff[167 + i]);
        c += 2;
    }

    printf("  Logical Unit Number: %s\n", uuid);

    /* Initiator transport ID */
    if ( buff[10] & 0x01 ) {
        memset(port_id, 0, 128);
        printf("  Transport Protocol: ");
        switch (buff[175] & 0x0F) {
        case TPROTO_FCP: /* FC */
            printf("FC\n");
            convert_binary_to_ascii(&buff[183], port_id, 8);
            n = 199;
            break;
        case TPROTO_SRP: /* SRP */
            printf("SRP\n");
            convert_binary_to_ascii(&buff[183], port_id, 8);
            n = 199;
            break;
        case TPROTO_ISCSI: /* iSCSI */
            printf("iSCSI\n");
            n = sg_get_unaligned_be32(buff + 177);
            memcpy(port_id, &buff[179], n);
            n = 179 + n;
            break;
        case TPROTO_SAS: /* SAS */
            printf("SAS\n");
            convert_binary_to_ascii(&buff[179], port_id, 8);
            n = 199;
            break;
        default:
            return; /* Can't continue decoding, so return */
        }

        printf("  Initiator Port Identifier: %s\n", port_id);
        if ( buff[10] & 0x02 ) {
            memset(port_id, 0, 128);
            memcpy(port_id, &buff[n], 8);
            printf("  Supplemental Vendor ID: %s\n", port_id);
        }
    }

    return;
}

#if 0
static void
decode_rdac_vpd_c9_rtpg_data(uint8_t aas, uint8_t vendor)
{
    printf("  Asymmetric Access State:");
    switch(aas & 0x0F) {
    case 0x0:
        printf(" Active/Optimized");
        break;
    case 0x1:
        printf(" Active/Non-Optimized");
        break;
    case 0x2:
        printf(" Standby");
        break;
    case 0x3:
        printf(" Unavailable");
        break;
    case 0xE:
        printf(" Offline");
        break;
    case 0xF:
        printf(" Transitioning");
        break;
    default:
        printf(" (unknown)");
        break;
    }
    printf("\n");

    printf("  Vendor Specific Field:");
    switch(vendor) {
    case 0x01:
        printf(" Operating normally");
        break;
    case 0x02:
        printf(" Non-responsive to queries");
        break;
    case 0x03:
        printf(" Controller being held in reset");
        break;
    case 0x04:
        printf(" Performing controller firmware download (1st controller)");
        break;
    case 0x05:
        printf(" Performing controller firmware download (2nd controller)");
        break;
    case 0x06:
        printf(" Quiesced as a result of an administrative request");
        break;
    case 0x07:
        printf(" Service mode as a result of an administrative request");
        break;
    case 0xFF:
        printf(" Details are not available");
        break;
    default:
        printf(" (unknown)");
        break;
    }
    printf("\n");
}

static void
decode_rdac_vpd_c9(uint8_t * buff, int len)
{
    if (len < 3) {
        pr2serr("Volume Access Control VPD page length too short=%d\n", len);
        return;
    }
    if (buff[4] != 'v' && buff[5] != 'a' && buff[6] != 'c') {
        pr2serr("Invalid page identifier %c%c%c%c, decoding not possible.\n",
                buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    if (buff[7] != '1') {
        pr2serr("Invalid page version '%c' (should be 1)\n", buff[7]);
    }
    if ( (buff[8] & 0xE0) == 0xE0 ) {
        printf("  IOShipping (ALUA): Enabled\n");
    } else {
        printf("  AVT:");
        if (buff[8] & 0x80) {
            printf(" Enabled");
            if (buff[8] & 0x40)
                printf(" (Allow reads on sector 0)");
            printf("\n");
        } else {
            printf(" Disabled\n");
        }
    }
    printf("  Volume Access via: ");
    if (buff[8] & 0x01)
        printf("primary controller\n");
    else
        printf("alternate controller\n");

    if (buff[8] & 0x08) {
        printf("  Path priority: %d ", buff[15] & 0xf);
        switch(buff[15] & 0xf) {
        case 0x1:
            printf("(preferred path)\n");
            break;
        case 0x2:
            printf("(secondary path)\n");
            break;
        default:
            printf("(unknown)\n");
            break;
        }

        printf("  Preferred Path Auto Changeable:");
        switch(buff[14] & 0x3C) {
        case 0x14:
            printf(" No (User Disabled and Host Type Restricted)\n");
            break;
        case 0x18:
            printf(" No (User Disabled)\n");
            break;
        case 0x24:
            printf(" No (Host Type Restricted)\n");
            break;
        case 0x28:
            printf(" Yes\n");
            break;
        default:
            printf(" (Unknown)\n");
            break;
        }

        printf("  Implicit Failback:");
        switch(buff[14] & 0x03) {
        case 0x1:
            printf(" Disabled\n");
            break;
        case 0x2:
            printf(" Enabled\n");
            break;
        default:
            printf(" (Unknown)\n");
            break;
        }
    } else {
        printf("  Path priority: %d ", buff[9] & 0xf);
        switch(buff[9] & 0xf) {
        case 0x1:
            printf("(preferred path)\n");
            break;
        case 0x2:
            printf("(secondary path)\n");
            break;
        default:
            printf("(unknown)\n");
            break;
        }
    }


    if (buff[8] & 0x80) {
        printf(" Target Port Group Data (This controller):\n");
        decode_rdac_vpd_c9_rtpg_data(buff[10], buff[11]);

        printf(" Target Port Group Data (Alternate controller):\n");
        decode_rdac_vpd_c9_rtpg_data(buff[12], buff[13]);
    }
}
#endif

static void
decode_rdac_vpd_ca(uint8_t * buff, int len)
{
    int i;

    if (len < 16) {
        pr2serr("Replicated Volume Source Identifier VPD page length too "
                "short=%d\n", len);
        return;
    }
    if (buff[4] != 'r' && buff[5] != 'v' && buff[6] != 's') {
        pr2serr("Invalid page identifier %c%c%c%c, decoding not possible.\n",
                buff[4], buff[5], buff[6], buff[7]);
        return;
    }
    if (buff[8] & 0x01) {
        printf("  Snapshot Volume\n");
        printf("  Base Volume WWID: ");
        for (i = 0; i < 16; i++)
            printf("%02x", buff[10 + i]);
        printf("\n");
    } else if (buff[8] & 0x02) {
        printf("  Copy Target Volume\n");
        printf("  Source Volume WWID: ");
        for (i = 0; i < 16; i++)
            printf("%02x", buff[10 + i]);
        printf("\n");
    } else
        printf(" Neither a snapshot nor a copy target volume\n");

    return;
}

static void
decode_rdac_vpd_d0(uint8_t * buff, int len)
{
    int i;

    if (len < 20) {
        pr2serr("Storage Array World Wide Name VPD page length too "
                "short=%d\n", len);
        return;
    }
    printf("  Storage Array WWN: ");
    for (i = 0; i < 16; i++)
        printf("%02x", buff[8 + i]);
    printf("\n");

    return;
}


static void
decode_dds_vpd_c0(uint8_t * buff, int len)
{
    char firmware_rev[25];
    char build_date[43];
    char hw_conf[21];
    char fw_conf[21];

    if (len < 0xb3) {
        pr2serr("Vendor-Unique Firmware revision page invalid length=%d\n",
                len);
        return;
    }
    memset(firmware_rev, 0x0, 25);
    memcpy(firmware_rev, &buff[5], 24);

    printf("  %s\n", firmware_rev);

    memset(build_date, 0x0, 43);
    memcpy(build_date, &buff[30], 42);

    printf("  %s\n", build_date);

    memset(hw_conf, 0x0, 21);
    memcpy(hw_conf, &buff[73], 20);
    printf("  %s\n", hw_conf);

    memset(fw_conf, 0x0, 21);
    memcpy(fw_conf, &buff[94], 20);
    printf("  %s\n", fw_conf);
    return;
}

static void
decode_hp_lto_vpd_cx(uint8_t * buff, int len, int page)
{
    char str[32];
    const char *comp = NULL;

    if (len < 0x5c) {
        pr2serr("Driver Component Revision Levels page invalid length=%d\n",
                len);
        return;
    }
    switch (page) {
        case 0xc0:
            comp = "Firmware";
            break;
        case 0xc1:
            comp = "Hardware";
            break;
        case 0xc2:
            comp = "PCA";
            break;
        case 0xc3:
            comp = "Mechanism";
            break;
        case 0xc4:
            comp = "Head Assy";
            break;
        case 0xc5:
            comp = "ACI";
            break;
    }
    if (!comp) {
        pr2serr("Driver Component Revision Level invalid page=0x%02x\n",
                page);
        return;
    }

    memset(str, 0x0, 32);
    memcpy(str, &buff[4], 26);
    printf("  %s\n", str);

    memset(str, 0x0, 32);
    memcpy(str, &buff[30], 19);
    printf("  %s\n", str);

    memset(str, 0x0, 32);
    memcpy(str, &buff[49], 24);
    printf("  %s\n", str);

    memset(str, 0x0, 32);
    memcpy(str, &buff[73], 23);
    printf("  %s\n", str);
    return;
}

static void
decode_ibm_lto_dcrl(uint8_t * buff, int len)
{
    if (len < 0x2b) {
        pr2serr("Driver Component Revision Levels page (IBM LTO) invalid "
                "length=%d\n", len);
        return;
    }
    printf("  Code name: %.12s\n", buff + 4);
    printf("  Time (hhmmss): %.7s\n", buff + 16);
    printf("  Date (yyyymmdd): %.8s\n", buff + 23);
    printf("  Platform: %.12s\n", buff + 31);
}

static void
decode_ibm_lto_dsn(uint8_t * buff, int len)
{
    if (len < 0x1c) {
        pr2serr("Driver Serial Numbers page (IBM LTO) invalid "
                "length=%d\n", len);
        return;
    }
    printf("  Manufacturing serial number: %.12s\n", buff + 4);
    printf("  Reported serial number: %.12s\n", buff + 16);
}

static void
decode_vpd_3_hit(uint8_t * b, int blen)
{
    uint16_t plen = sg_get_unaligned_be16(b + 2);

    if ((plen < 184) || (blen < 184)) {
        pr2serr("Hitachi VPD page 0x3 length (%u) shorter than %u\n",
                plen + 4, 184 + 4);
        return;
    }
    printf("  ASCII uCode Identifier: %.12s\n", b + 24);
    printf("  ASCII servo P/N: %.4s\n", b + 36);
    printf("  Major Version: %.2s\n", b + 40);
    printf("  Minor Version: %.2s\n", b + 42);
    printf("  User Count: %.4s\n", b + 44);
    printf("  Build Number: %.4s\n", b + 48);
    printf("  Build Date String: %.32s\n", b + 52);
    printf("  Product ID: %.8s\n", b + 84);
    printf("  Interface ID: %.8s\n", b + 92);
    printf("  Code Type: %.8s\n", b + 100);
    printf("  User Name: %.12s\n", b + 108);
    printf("  Machine Name: %.16s\n", b + 120);
    printf("  Directory Name: %.32s\n", b + 136);
    printf("  Operating state: %u\n", sg_get_unaligned_be32(b + 168));
    printf("  Functional Mode: %u\n", sg_get_unaligned_be32(b + 172));
    printf("  Degraded Reason: %u\n", sg_get_unaligned_be32(b + 176));
    printf("  Broken Reason: %u\n", sg_get_unaligned_be32(b + 180));
    printf("  Code Mode: %u\n", sg_get_unaligned_be32(b + 184));
    printf("  Revision: %.4s\n", b + 188);
}

static void
decode_vpd_d1_hit(uint8_t * b, int blen)
{
    uint16_t plen = sg_get_unaligned_be16(b + 2);

    if ((plen < 80) || (blen < 80)) {
        pr2serr("Hitachi VPD page 0xd1 length (%u) shorter than %u\n",
                plen + 4, 80 + 4);
        return;
    }
    printf("  ASCII Media Disk Definition: %.16s\n", b + 4);
    printf("  ASCII Motor Serial Number: %.16s\n", b + 20);
    printf("  ASCII Flex Assembly Serial Number: %.16s\n", b + 36);
    printf("  ASCII Actuator Serial Number: %.16s\n", b + 52);
    printf("  ASCII Device Enclosure Serial Number: %.16s\n", b + 68);
}

static void
decode_vpd_d2_hit(uint8_t * b, int blen)
{
    uint16_t plen = sg_get_unaligned_be16(b + 2);

    if ((plen < 52) || (blen < 52)) {
        pr2serr("Hitachi VPD page 0xd2 length (%u) shorter than %u\n",
                plen + 4, 52 + 4);
        return;
    }
    if ((blen - 4) == 120) {
        printf("  HDC Version: %.*s\n", b[4], b + 5);
        printf("  Card Serial Number: %.*s\n", b[24], b + 25);
        printf("  NAND Flash Version: %.*s\n", b[44], b + 45);
        printf("  Card Assembly Part Number: %.*s\n", b[64], b + 65);
        printf("  Second Card Serial Number: %.*s\n", b[84], b + 85);
        printf("  Second Card Assembly Part Number: %.*s\n", b[104], b + 105);
    } else {
        printf("  ASCII HDC Version: %.16s\n", b + 5);
        printf("  ASCII Card Serial Number: %.16s\n", b + 22);
        printf("  ASCII Card Assembly Part Number: %.16s\n", b + 39);
    }
}

/* Returns 0 if successful, see sg_ll_inquiry() plus SG_LIB_CAT_OTHER for
   unsupported page */
int
svpd_decode_vendor(struct sg_pt_base * ptvp, struct opts_t * op,
		   sgj_opaque_p jop, int off)
{
    bool as_json;
    int len, pdt, pn, dhex;
    int alloc_len = op->maxlen;
    int res = 0;
    const struct svpd_values_name_t * vnp;
    sgj_state * jsp = &op->json_st;
    sgj_opaque_p jo2p = NULL;
    uint8_t * rp;
    char name[80];

    as_json = jsp->pr_as_json;
    dhex = op->do_hex;
    if (dhex < 0)
	dhex = -dhex;
    pn = op->vpd_pn;

    switch (pn) {       /* VPD codes that we support vendor pages for */
    case 0x3:
    case 0xc0:
    case 0xc1:
    case 0xc2:
    case 0xc3:
    case 0xc4:
    case 0xc5:
    case 0xc8:
    case 0xc9:
    case 0xca:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xde:
        break;
    default:    /* not known so return prior to fetching page */
        return SG_LIB_CAT_OTHER;
    }
    rp = rsp_buff + off;
    if (ptvp) {
        if (0 == alloc_len)
            alloc_len = DEF_ALLOC_LEN;
    }
    res = vpd_fetch_page(ptvp, rp, pn, alloc_len, op->do_quiet, op->verbose,
                         &len);
    if (res) {
        pr2serr("Vendor VPD page=0x%x  failed to fetch\n", pn);
        return res;
    }
    pdt = rp[0] & PDT_MASK;
    vnp = svpd_get_v_detail(pn, op->vend_prod_num, pdt);
    if (vnp && vnp->name)
        snprintf(name, sizeof(name), "%s", vnp->name);
    else
        snprintf(name, sizeof(name) - 1, "Vendor VPD page=0x%x", pn);
    if ((! op->do_raw) && (! op->do_quiet) && (op->do_hex < 3))
        sgj_pr_hr(jsp, "%s VPD Page:\n", name);
    if (op->do_raw)
        dStrRaw(rp, len);
    else {
        switch(pn) {
        case 0x3:
            if ((0 == dhex) && (VPD_VP_WDC_HITACHI == op->vend_prod_num))
                decode_vpd_3_hit(rp, len);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc0:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_SEAGATE == op->vend_prod_num)
                decode_firm_vpd_c0_sea(rp, len);
            else if (VPD_VP_EMC == op->vend_prod_num) {
                if (as_json)
                    jo2p = sg_vpd_js_hdr(jsp, jop,
                                         "Unit serial number VPD page", rp);
                decode_upr_vpd_c0_emc(rp, len, op, jo2p);
            } else if (VPD_VP_HP3PAR == op->vend_prod_num)
                decode_vpd_c0_hp3par(rp, len);
            else if (VPD_VP_RDAC == op->vend_prod_num)
                decode_rdac_vpd_c0(rp, len);
            else if (VPD_VP_DDS == op->vend_prod_num)
                decode_dds_vpd_c0(rp, len);
            else if (VPD_VP_IBM_LTO == op->vend_prod_num)
                decode_ibm_lto_dcrl(rp, len);
            else if (VPD_VP_HP_LTO == op->vend_prod_num)
                decode_hp_lto_vpd_cx(rp, len, pn);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc1:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_SEAGATE == op->vend_prod_num)
                decode_date_code_vpd_c1_sea(rp, len);
            else if (VPD_VP_RDAC == op->vend_prod_num)
                decode_rdac_vpd_c1(rp, len);
            else if (VPD_VP_IBM_LTO == op->vend_prod_num)
                decode_ibm_lto_dsn(rp, len);
            else if (VPD_VP_HP_LTO == op->vend_prod_num)
                decode_hp_lto_vpd_cx(rp, len, pn);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc2:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_RDAC == op->vend_prod_num) {
                if (as_json)
                    jo2p = sg_vpd_js_hdr(jsp, jop,
                                         "Software version VPD page", rp);
                decode_rdac_vpd_c2(rp, len, op, jo2p);
            } else if (VPD_VP_HP_LTO == op->vend_prod_num)
                decode_hp_lto_vpd_cx(rp, len, pn);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc3:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_SEAGATE == op->vend_prod_num)
                decode_dev_beh_vpd_c3_sea(rp, len);
            else if (VPD_VP_RDAC == op->vend_prod_num)
                decode_rdac_vpd_c3(rp, len);
            else if (VPD_VP_HP_LTO == op->vend_prod_num)
                decode_hp_lto_vpd_cx(rp, len, pn);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc4:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_RDAC == op->vend_prod_num)
                decode_rdac_vpd_c4(rp, len);
            else if (VPD_VP_HP_LTO == op->vend_prod_num)
                decode_hp_lto_vpd_cx(rp, len, pn);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc5:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_HP_LTO == op->vend_prod_num)
                decode_hp_lto_vpd_cx(rp, len, pn);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc8:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_RDAC == op->vend_prod_num)
                decode_rdac_vpd_c8(rp, len);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xc9:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_RDAC == op->vend_prod_num) {
                if (as_json)
                    jo2p = sg_vpd_js_hdr(jsp, jop,
                                         "Volume access control VPD page", rp);
                decode_rdac_vpd_c9(rp, len, op, jo2p);
            } else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xca:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_RDAC == op->vend_prod_num)
                decode_rdac_vpd_ca(rp, len);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xd0:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_RDAC == op->vend_prod_num)
                decode_rdac_vpd_d0(rp, len);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xd1:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_WDC_HITACHI == op->vend_prod_num)
                decode_vpd_d1_hit(rp, len);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        case 0xd2:
            if (dhex > 0) {
		if (dhex > 2)
		    named_hhh_output(NULL, rp, len, op);
		else
                    hex2stdout(rp, len, no_ascii_4hex(op));
            } else if (VPD_VP_WDC_HITACHI == op->vend_prod_num)
                decode_vpd_d2_hit(rp, len);
            else
                res = SG_LIB_CAT_OTHER;
            break;
        default:
            res = SG_LIB_CAT_OTHER;
        }
    }
    if (res && (op->verbose > 1))
        pr2serr("%s: can't decode pn=0x%x, vend_prod_num=%d\n", __func__,
                pn, op->vend_prod_num);
    return res;
}
