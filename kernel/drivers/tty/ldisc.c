#include "drivers/tty/ldisc.h"
#include <drivers/keyboard.h>
#include <drivers/tty/tty.h>
#include <errno.h>
#include <util/bits.h>
#include <util/debug.h>
#include <util/string.h>
#include <drivers/tty/vterminal.h>

#define ldisc_to_tty(ldisc) CONTAINER_OF((ldisc), tty_t, tty_ldisc)

/**
 * Initialize the line discipline. Don't forget to wipe the buffer associated
 * with the line discipline clean.
 *
 * @param ldisc line discipline.
 */
void ldisc_init(ldisc_t *ldisc)
{
    ldisc->ldisc_cooked=0; // cooked == tail == head when empty
    ldisc->ldisc_tail=0;
    ldisc->ldisc_head=0;
    ldisc->ldisc_full=0; // Not NULL
    sched_queue_init(&ldisc->ldisc_read_queue); // Initialize read queue
    memset(ldisc->ldisc_buffer,'\0',LDISC_BUFFER_SIZE); // Clean the buffer
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_init");
}

/**
 * While there are no new characters to be read from the line discipline's
 * buffer, you should make the current thread to sleep on the line discipline's
 * read queue. Note that this sleep can be cancelled. What conditions must be met 
 * for there to be no characters to be read?
 *
 * @param  ldisc the line discipline
 * @param  lock  the lock associated with `ldisc`
 * @return       0 if there are new characters to be read or the ldisc is full.
 *               If the sleep was interrupted, return what
 *               `sched_cancellable_sleep_on` returned (i.e. -EINTR)
 */
long ldisc_wait_read(ldisc_t *ldisc, spinlock_t *lock)
{
    // Condition: When there are no cooked characters in buffer
    long ret;
    // If it is full, it must has something to read because it should be cooked
    // When the tail is equal to cooked and it is not full, which means that there are no cooked characters
    while(ldisc->ldisc_tail ==ldisc->ldisc_cooked && (!ldisc->ldisc_full)){      
      ret=sched_cancellable_sleep_on(&ldisc->ldisc_read_queue,lock); // Put current thread into sleep
      if(ret<0){  // If we face an error condition
        return ret;
      }
    }   
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_wait_read");
    return ret;
}

/**
 * Reads `count` bytes (at max) from the line discipline's buffer into the
 * provided buffer. Keep in mind the the ldisc's buffer is circular.
 *
 * If you encounter a new line symbol before you have read `count` bytes, you
 * should stop copying and return the bytes read until now. We also need to put it into buf
 * 
 * If you encounter an `EOT` you should stop reading and you should NOT include 
 * the `EOT` in the count of the number of bytes read
 *
 * @param  ldisc the line discipline
 * @param  buf   the buffer to read into.
 * @param  count the maximum number of bytes to read from ldisc.
 * @return       the number of bytes read from the ldisc.
 */
size_t ldisc_read(ldisc_t *ldisc, char *buf, size_t count)
{
    // TODO: Find a way to lock the buffer
    size_t cur_count=0; // Compute the current count in provided buffer buf
    // If the ldisc_cooked didn't reach the end of the buffer
    // if(ldisc->ldisc_tail ==ldisc->ldisc_cooked && (ldisc->ldisc_full==1)){
    //     return cur_count;
    // }
    for(cur_count=0;cur_count<=count;cur_count++){
        if(ldisc->ldisc_buffer[ldisc->ldisc_tail]==EOT){
            ldisc->ldisc_tail=(ldisc->ldisc_tail+1)%LDISC_BUFFER_SIZE;
            if(ldisc->ldisc_full==1){
                ldisc->ldisc_full=0;
            }  
            return cur_count;        
        }
        else if(ldisc->ldisc_buffer[ldisc->ldisc_tail]=='\n'){
            buf[cur_count]=ldisc->ldisc_buffer[ldisc->ldisc_tail%LDISC_BUFFER_SIZE];
            ldisc->ldisc_tail=(ldisc->ldisc_tail+1)%LDISC_BUFFER_SIZE;          
            cur_count++;
            if(ldisc->ldisc_full==1){
                ldisc->ldisc_full=0;
            }  
            return cur_count;
        }
        else{
            buf[cur_count]=ldisc->ldisc_buffer[ldisc->ldisc_tail];
            ldisc->ldisc_tail=(ldisc->ldisc_tail+1)%LDISC_BUFFER_SIZE;
            if(ldisc->ldisc_full==1){
                ldisc->ldisc_full=0;
            }  
        }
    }
    return cur_count;
}


/**
 * Place the character received into the ldisc's buffer. You should also update
 * relevant fields of the struct.
 *
 * An easier way of handling new characters is making sure that you always have
 * one byte left in the line discipline. This way, if the new character you
 * received is a new line symbol (user hit enter), you can still place the new
 * line symbol into the buffer; if the new character is not a new line symbol,
 * you shouldn't place it into the buffer so that you can leave the space for
 * a new line symbol in the future. 
 * 
 *
 * If the line discipline is almost full, i.e. the case where there is one slot 
 * left in the line discipline buffer, the only characters that can be handled are:
 * EOT --> ctrl-D
 * \n --> newline or LF 
 * ETX --> ctrl-C
 * \b --> backspace
 * Here are some special cases to consider:
 *      1. If the character is a backspace:
 *          * if there is a character to remove you must also emit a `\b` to
 *            the vterminal.
 *      2. If the character is end of transmission (EOT) character (typing ctrl-d)
 *      3. If the character is end of text (ETX) character (typing ctrl-c)
 *      4. If your buffer is almost full and what you received is not a new line
 *      symbol
 *
 * If you did receive a new line symbol, you should wake up the thread that is
 * sleeping on the wait queue of the line discipline. You should also
 * emit a `\n` to the vterminal by using `vterminal_write`.  
 * 
 * If you encounter the `EOT` character, you should add it to the buffer, 
 * cook the buffer, and wake up the reader (but do not emit an `\n` character 
 * to the vterminal)
 * 
 * In case of `ETX` you should cause the input line to be effectively transformed
 * into a cooked blank line. You should clear uncooked portion of the line, by 
 * adjusting ldisc_head. 
 * Clarification:In the case of an ETX, you would want to remove the uncooked part and 
 * have a '\n' written on the terminal.On the terminal, the cursor should just move onto 
 * the next line without executing anything that was typed before.
 *
 * Finally, if the none of the above cases apply you should fallback to
 * `vterminal_key_pressed`.
 *
 * Don't forget to write the corresponding characters to the virtual terminal
 * when it applies!
 *
 * @param ldisc the line discipline
 * @param c     the new character
 */
void ldisc_key_pressed(ldisc_t *ldisc, char c)
{
    if(ldisc->ldisc_full==1){ // If the buffer is full
        return;      
    }
    // If we encounter special characters, this type of character should be handled by normal and almost full
    // situation
    if(c==EOT||c==ETX||c=='\n'||c=='\b'){ 
        if (c==EOT) {  // If we encounter Ctrl+D, It should execute whatever was typed before, but not move to next line
            ldisc->ldisc_buffer[ldisc->ldisc_head]=c; // Put EOT into buffer
            ldisc->ldisc_head=(ldisc->ldisc_head+1)%LDISC_BUFFER_SIZE;
            ldisc->ldisc_cooked=ldisc->ldisc_head; // Cook the buffer
            sched_wakeup_on(&ldisc->ldisc_read_queue,NULL); // Begin to read
        }
        else if (c==ETX) { // We need to discard all the element we put into buffer
            ldisc->ldisc_head=ldisc->ldisc_cooked; // Discard the raw item
            vterminal_write(&ldisc_to_tty(ldisc)->tty_vterminal,"\n",1); // Output \n to the terminal
        }
        else if(c=='\n'){ // When we encounter next line
            ldisc->ldisc_buffer[ldisc->ldisc_head]=c;
            ldisc->ldisc_head=(ldisc->ldisc_head+1)%LDISC_BUFFER_SIZE;
            ldisc->ldisc_cooked=ldisc->ldisc_head; // Set it as cooked
            sched_wakeup_on(&ldisc->ldisc_read_queue,NULL);
            vterminal_write(&ldisc_to_tty(ldisc)->tty_vterminal,"\n",1);
        }
        else if(c=='\b'){ // When we encounter backspace, remove the last character pressed
        // If the head has move to the first position in the buffer and the raw content is not empty
            if(ldisc->ldisc_head!=ldisc->ldisc_cooked){ // Ensure the raw part is not empty
                if(ldisc->ldisc_head==0 ){ 
                // We don't need to replace the deleted element's content with 0
                    ldisc->ldisc_head=LDISC_BUFFER_SIZE-1; // Update the value of head
                }
                else { // The head is not in the first element of buffer
                    ldisc->ldisc_head--;
                }
                vterminal_write(&ldisc_to_tty(ldisc)->tty_vterminal,"\b",1); // Write '\b' to terminal
            }
        }
        // If the head has beyond the range of buffer, we need to update the value of head
    }
    // This condition check that the buffer is not full, which means that the head+1 didn't hit
    // the tail(Raw part is from cooked(include) to head(not include), cooked part is from tail
    // (included) to cooked(not included, if the head is just near the tail, which means that it's alomost full)
    else if((ldisc->ldisc_head+1) % LDISC_BUFFER_SIZE != ldisc->ldisc_tail){  // Normal situation, 
        // when it is not almost full, which means that there are at least two bytes left
        ldisc->ldisc_buffer[ldisc->ldisc_head]=c;
        ldisc->ldisc_head=(ldisc->ldisc_head+1)%LDISC_BUFFER_SIZE;
        vterminal_key_pressed(&ldisc_to_tty(ldisc)->tty_vterminal);
    }
    // To handle the situation in which the buffer is almost full and it received EOT and '\n'
    // which means there are only one byte left in the line discipline. After receive EOT and
    // '\n', it's completely full.
    if (ldisc->ldisc_head == ldisc->ldisc_tail &&(c==EOT||c=='\n')) {
        ldisc->ldisc_full = 1;
    }
    return; 
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_key_pressed");
}

/**
 * Copy the raw part of the line discipline buffer into the buffer provided.
 *
 * @param  ldisc the line discipline
 * @param  s     the character buffer to write to
 * @return       the number of bytes copied
 */
size_t ldisc_get_current_line_raw(ldisc_t *ldisc, char *s)
{
    size_t cur_count=0;
    // If (ldisc->ldisc_cooked+cur_count)%LDISC_BUFFER_SIZE==ldisc->ldisc_head, which means that it is already full
    // If it is full, the only possible byte are EOT and '\n', we don't need to get it
    for(cur_count=0;(ldisc->ldisc_cooked+cur_count)%LDISC_BUFFER_SIZE!=ldisc->ldisc_head;cur_count++){
        s[cur_count]=ldisc->ldisc_buffer[(ldisc->ldisc_cooked+cur_count)%LDISC_BUFFER_SIZE]; // Add it into buffer 
    }
   // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_get_current_line_raw");
    return cur_count;
}
