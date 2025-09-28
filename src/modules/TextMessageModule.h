#pragma once
#include "Observer.h"
#include "SinglePortModule.h"

/**
 * Text message handling for meshtastic - draws on the OLED display the most recent received message
 */
class TextMessageModule : public SinglePortModule, public Observable<const meshtastic_MeshPacket *>
{
  public:
    /** Constructor
     * name is for debugging output
     */
    TextMessageModule() : SinglePortModule("text", meshtastic_PortNum_TEXT_MESSAGE_APP) {}
    

  protected:
    /** Called to handle a particular incoming message

    @return ProcessMessage::STOP if you've guaranteed you've handled this message and no other handlers should be considered for
    it
    */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    //fw+ take packet by const reference; expose default targetId at declaration
    virtual void sendTextMessage(const std::string &message, const meshtastic_MeshPacket &mp, uint32_t targetId = 0);
    std::string formatFloatToOneDecimal(float value);
};

extern TextMessageModule *textMessageModule;