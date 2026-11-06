// Whether Windows' firewall lets headsets reach the streamer on the networks in use.
#pragma once

#include <string>

namespace rcs::firewall {

struct Status {
    bool otherNetworksOpen = true;  // Private and Domain networks in use let headsets in
    bool publicInUse = false;       // this PC is on a network that's set as Public (the USB cable aside)
    bool publicAllowed = false;     // headsets can get in on Public networks
    bool publicUnguarded = false;   // because the firewall itself doesn't guard them
    std::string usbAdapter;         // a headset on a USB cable: Windows' name for that adapter
    bool usbOpen = true;            // headsets can get in over it
};

// Takes a moment when there are many rules, so not for the UI thread.
Status Query(const std::wstring& exe);

// Lets the program through on Private and Domain networks, and on Public ones if asked. Shows
// Windows' permission prompt. False if that was declined or failed.
bool Set(const std::wstring& exe, bool allowPublic);

// A USB cable to a headset is an "unidentified network" to Windows, so Public. Lets the program
// through on that adapter alone (added to those allowed before), not on every Public network.
bool AllowUsbAdapter(const std::wstring& exe, const std::string& adapter);

}  // namespace rcs::firewall
