#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    CGKeyCode key_code;
    bool key_down;
    CGEventFlags flags;
} KeyStep;

static const useconds_t delay_micros = 25000;

static bool accessibility_is_trusted(bool prompt) {
    const void *keys[] = { kAXTrustedCheckOptionPrompt };
    const void *values[] = { prompt ? kCFBooleanTrue : kCFBooleanFalse };
    CFDictionaryRef options = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        1,
        &kCFCopyStringDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    bool trusted = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
    return trusted;
}

static void post_key_step(CGEventSourceRef source, KeyStep step) {
    CGEventRef event = CGEventCreateKeyboardEvent(source, step.key_code, step.key_down);
    if (!event) {
        return;
    }

    CGEventSetFlags(event, step.flags);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    usleep(delay_micros);
}

static void tap_function_key(CGEventSourceRef source) {
    const CGKeyCode function_key = 63;

    post_key_step(source, (KeyStep){function_key, true, kCGEventFlagMaskSecondaryFn});
    post_key_step(source, (KeyStep){function_key, false, 0});
}

static void press_function_key(CGEventSourceRef source) {
    const CGKeyCode function_key = 63;

    post_key_step(source, (KeyStep){function_key, true, kCGEventFlagMaskSecondaryFn});
}

static void release_function_key(CGEventSourceRef source) {
    const CGKeyCode function_key = 63;

    post_key_step(source, (KeyStep){function_key, false, 0});
}

static void press_dictation_combo(CGEventSourceRef source) {
    const CGKeyCode left_shift = 56;
    const CGKeyCode left_command = 55;
    const CGKeyCode left_control = 59;
    const CGKeyCode space = 49;

    post_key_step(source, (KeyStep){left_control, true, kCGEventFlagMaskControl});
    post_key_step(source, (KeyStep){
        left_command,
        true,
        kCGEventFlagMaskControl | kCGEventFlagMaskCommand,
    });
    post_key_step(source, (KeyStep){
        left_shift,
        true,
        kCGEventFlagMaskControl | kCGEventFlagMaskCommand | kCGEventFlagMaskShift,
    });
    post_key_step(source, (KeyStep){
        space,
        true,
        kCGEventFlagMaskControl | kCGEventFlagMaskCommand | kCGEventFlagMaskShift,
    });
}

static void release_dictation_combo(CGEventSourceRef source) {
    const CGKeyCode left_shift = 56;
    const CGKeyCode left_command = 55;
    const CGKeyCode left_control = 59;
    const CGKeyCode space = 49;

    post_key_step(source, (KeyStep){
        space,
        false,
        kCGEventFlagMaskControl | kCGEventFlagMaskCommand | kCGEventFlagMaskShift,
    });
    post_key_step(source, (KeyStep){
        left_shift,
        false,
        kCGEventFlagMaskControl | kCGEventFlagMaskCommand,
    });
    post_key_step(source, (KeyStep){left_command, false, kCGEventFlagMaskControl});
    post_key_step(source, (KeyStep){left_control, false, 0});
}

static void tap_dictation_combo(CGEventSourceRef source) {
    press_dictation_combo(source);
    release_dictation_combo(source);
}

static int print_usage(void) {
    fputs("Usage: send_typeless_key check|request-accessibility|fn|fn-down|fn-up|ctrl-cmd-shift-space|ctrl-cmd-shift-space-down|ctrl-cmd-shift-space-up\n", stderr);
    return 64;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return print_usage();
    }

    if (strcmp(argv[1], "check") == 0) {
        printf("accessibility_trusted=%s\n", accessibility_is_trusted(false) ? "true" : "false");
        return 0;
    }

    if (strcmp(argv[1], "request-accessibility") == 0) {
        bool trusted = accessibility_is_trusted(true);
        printf("accessibility_trusted=%s\n", trusted ? "true" : "false");
        return trusted ? 0 : 70;
    }

    if (!accessibility_is_trusted(false)) {
        fputs("accessibility_trusted=false\n", stderr);
        return 70;
    }

    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (source) {
        CGEventSourceSetLocalEventsSuppressionInterval(source, 0);
    }

    if (strcmp(argv[1], "fn") == 0) {
        tap_function_key(source);
    } else if (strcmp(argv[1], "fn-down") == 0) {
        press_function_key(source);
    } else if (strcmp(argv[1], "fn-up") == 0) {
        release_function_key(source);
    } else if (strcmp(argv[1], "ctrl-cmd-shift-space") == 0) {
        tap_dictation_combo(source);
    } else if (strcmp(argv[1], "ctrl-cmd-shift-space-down") == 0) {
        press_dictation_combo(source);
    } else if (strcmp(argv[1], "ctrl-cmd-shift-space-up") == 0) {
        release_dictation_combo(source);
    } else {
        if (source) {
            CFRelease(source);
        }
        return print_usage();
    }

    if (source) {
        CFRelease(source);
    }
    return 0;
}
