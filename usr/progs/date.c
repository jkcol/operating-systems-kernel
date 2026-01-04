#include "syscall.h"
#include <stdint.h>
#include <stdio.h>

// helper
void find_time(int total_time, int out[6]);

void main(int argc, char* argv[]){
    int ret;
    unsigned long long time_buf;
    char * month_names[] = {"January", "February", "March", "April", "May", "June",
                            "July", "August", "September", "October", "November", "December"};
    
    // open the rtc device as a file
    _close(0);
    ret = _open(0, "dev/rtc0");
    if(ret < 0){
        printf("Unable to open RTC\n");
        return;
    }

    // rtc returns the timestamp and puts it in time_buf
    _read(ret, &time_buf, sizeof(uint64_t));

    // get the returned time and cast it as int
    int out[6];
    unsigned long long time = time_buf;
    int toal_time = time/1000000000ULL; // convert from ns to s 

    // calculate current time
    find_time(toal_time, out);

    // create a buffer with our message and write it to STDOUT
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%02d %s %04d %02d:%02d:%02d", out[2], month_names[out[1]-1], out[0], out[3], out[4], out[5]);
    _write(1, buf, len);
}   

void find_time(int total_time, int out[6]){
    
    // get total days & seconds
    int days = total_time / 86400;
    int sod = total_time % 86400;
    int hour = sod / 3600;
    int minute = (sod % 3600) / 60;
    int second = sod % 60;

    // find year
    int year = 1970;
    int diy;
    for(;;){
        if(year % 4 == 0){
            diy = 366; // leap year
        }
        else{
            diy = 365;
        }
        if(days >= diy){
            days -= diy;
            year += 1;
        }
        else{
            break;
        }
    }

    // find month
    int feb_days = 28;
    if(year % 4 == 0){  
        feb_days = 29;
    }
    int month_legends[] = {31, feb_days, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month = 1;
    for(int m = 0; m < 12; m++){
        if(days >= month_legends[m]){
            days -= month_legends[m];
            month += 1;
        }
        else{
            break;
        }
    }

    days = days + 1;

    out[0] = year;
    out[1] = month;
    out[2] = days;
    out[3] = hour;
    out[4] = minute;
    out[5] = second;
}