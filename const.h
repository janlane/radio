#ifndef RADIO_CONST_H
#define RADIO_CONST_H

#include <sys/types.h>

/* first chars of LOOKUP_MSG and REXMIT_MSG shall remain different */
#define TOP "------------------------------------------------------------------------\r\n  SIK Radio\r\n------------------------------------------------------------------------\r\n"
#define FOOT "------------------------------------------------------------------------\r\n"
#define CHOICE "  > "
#define NO_CHOICE "    "
#define LOOKUP_MSG "ZERO_SEVEN_COME_IN\n"
#define REXMIT_MSG "LOUDER_PLEASE "
#define REPLY_MSG "BOREWICZ_HERE"
static const int TOP_LEN = 3; // number of lines in TOP string
static const int FOOT_LEN = 1; // number of lines in FOOT string
static const size_t MAX_UDP_MSG_LEN = 65536;
static const size_t MAX_CTRL_MSG_LEN = 128;
static const size_t LOOKUP_MSG_LEN = 19;
static const size_t MAX_NAME_LEN = 64;


#endif //RADIO_CONST_H
