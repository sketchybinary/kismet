/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "config.h"

#include "phy_rtlamr.h"
#include "devicetracker.h"
#include "kismet_json.h"
#include "endian_magic.h"
#include "macaddr.h"
#include "kis_httpd_registry.h"
#include "manuf.h"

Kis_RTLAMR_Phy::Kis_RTLAMR_Phy(global_registry *in_globalreg, int in_phyid) :
    kis_phy_handler(in_globalreg, in_phyid) {

    set_phy_name("RTLAMR");

    packetchain =
        Globalreg::fetch_mandatory_global_as<packet_chain>();
    entrytracker =
        Globalreg::fetch_mandatory_global_as<entry_tracker>();
    devicetracker =
        Globalreg::fetch_mandatory_global_as<device_tracker>();

	pack_comp_common = 
		packetchain->register_packet_component("COMMON");
    pack_comp_json = 
        packetchain->register_packet_component("JSON");
    pack_comp_meta =
        packetchain->register_packet_component("METABLOB");

    rtlamr_holder_id =
        Globalreg::globalreg->entrytracker->register_field("rtlamr.device", 
                tracker_element_factory<tracker_element_map>(),
                "rtl_amr device");

    rtlamr_common_id =
        Globalreg::globalreg->entrytracker->register_field("rtlamr.device.common",
                tracker_element_factory<rtlamr_tracked_common>(),
                "Common RTLAMR device info");

    rtlamr_powermeter_id =
        Globalreg::globalreg->entrytracker->register_field("rtlamr.device.powermeter",
                tracker_element_factory<rtlamr_tracked_powermeter>(),
                "RTLAMR powermeter");

    // Make the manuf string
    rtl_manuf = Globalreg::globalreg->manufdb->MakeManuf("RTLAMR");

    // Register js module for UI
    auto httpregistry = Globalreg::fetch_mandatory_global_as<kis_httpd_registry>();
    httpregistry->register_js_module("kismet_ui_rtlamr", "js/kismet.ui.rtlamr.js");

	packetchain->register_handler(&PacketHandler, this, CHAINPOS_CLASSIFIER, -100);
}

Kis_RTLAMR_Phy::~Kis_RTLAMR_Phy() {
    packetchain->remove_handler(&PacketHandler, CHAINPOS_CLASSIFIER);
}


mac_addr Kis_RTLAMR_Phy::json_to_mac(Json::Value json) {
    // Derive a mac addr from the model and device id data
    //
    // We turn the model string into 4 bytes using the adler32 checksum,
    // then we use the model as a (potentially) 16bit int
    //
    // Finally we set the locally assigned bit on the first octet

    uint8_t bytes[6];
    uint16_t *model = (uint16_t *) bytes;
    uint32_t *checksum = (uint32_t *) (bytes + 2);

    memset(bytes, 0, 6);

    std::string smodel = "unk";

    if (json.isMember("model")) {
        Json::Value m = json["model"];
        if (m.isString()) {
            smodel = m.asString();
        }
    }

    *checksum = adler32_checksum(smodel.c_str(), smodel.length());

    bool set_model = false;
    if (json.isMember("Message")) {
      auto msgjson = json["Message"];
      if (msgjson.isMember("ID")) {
          Json::Value i = msgjson["ID"];
          if (i.isNumeric()) {
              *model = kis_hton16((uint16_t) i.asUInt());
              set_model = true;
          }
      }
    }
  
    if (!set_model) {
        *model = 0x0000;
    }

    // Set the local bit
    bytes[0] |= 0x2;

    return mac_addr(bytes, 6);
}

bool Kis_RTLAMR_Phy::json_to_rtl(Json::Value json, kis_packet *packet) {
    std::string err;
    std::string v;

    // synth a mac out of it
    mac_addr rtlmac = json_to_mac(json);

    if (rtlmac.error) {
        return false;
    }

    kis_common_info *common = 
        (kis_common_info *) packet->fetch(pack_comp_common);

    if (common == NULL) {
        common = new kis_common_info;
        packet->insert(pack_comp_common, common);
    }

    common->type = packet_basic_data;
    common->phyid = fetch_phy_id();
    common->datasize = 0;

    // If this json record has a channel
    if (json.isMember("channel")) {
        Json::Value c = json["channel"];
        if (c.isNumeric()) {
            common->channel = int_to_string(c.asInt());
        } else if (c.isString()) {
            common->channel = munge_to_printable(c.asString());
        }
    }

    common->freq_khz = 912600;
    common->source = rtlmac;
    common->transmitter = rtlmac;

    std::shared_ptr<kis_tracked_device_base> basedev =
        devicetracker->update_common_device(common, common->source, this, packet,
                (UCD_UPDATE_FREQUENCIES | UCD_UPDATE_PACKETS | UCD_UPDATE_LOCATION |
                 UCD_UPDATE_SEENBY), "RTLAMR Sensor");

    local_locker bssidlock(&(basedev->device_mutex));

    std::string dn = "PowerMeter";

    if (json.isMember("Message")) {
      auto msgjson = json["Message"];
      if (msgjson.isMember("ID")) {
          Json::Value i = msgjson["ID"];
          if (i.isNumeric()) {
              dn = i.asString();
          }
      }
    }

    basedev->set_manuf(rtl_manuf);

    basedev->set_type_string("Power Meter");
    basedev->set_devicename(dn);

    auto rtlholder = basedev->get_sub_as<tracker_element_map>(rtlamr_holder_id);
    bool newrtl = false;

    if (rtlholder == NULL) {
        rtlholder =
            std::make_shared<tracker_element_map>(rtlamr_holder_id);
        basedev->insert(rtlholder);
        newrtl = true;
    }

    auto commondev =
        rtlholder->get_sub_as<rtlamr_tracked_common>(rtlamr_common_id);

    if (commondev == NULL) {
        commondev =
            std::make_shared<rtlamr_tracked_common>(rtlamr_common_id);
        rtlholder->insert(commondev);

        commondev->set_model(dn);

        bool set_id = false;
        if (json.isMember("Message")) {
	  auto msgjson = json["Message"];
          if (msgjson.isMember("ID")) {
              Json::Value id_j = msgjson["ID"];
              if (id_j.isNumeric()) {
                  std::stringstream ss;
                  ss << id_j.asString();
                  commondev->set_rtlid(ss.str());
                  set_id = true;
              } else if (id_j.isString()) {
                  commondev->set_rtlid(id_j.asString());
                  set_id = true;
              }
          }

          if (!set_id && msgjson.isMember("Consumption")) {
              Json::Value consumption_j = msgjson["Consumption"];
              if (consumption_j.isNumeric()) {
                  std::stringstream ss;
                  ss << consumption_j.asDouble();
                  commondev->set_rtlid(ss.str());
                  set_id = true;
              } else if (consumption_j.isString()) {
                  commondev->set_rtlid(consumption_j.asString());
                  set_id = true;
              }
          }
        }

        if (!set_id) {
            commondev->set_rtlid("");
        }

        commondev->set_rtlchannel("0");
    }

    if (json.isMember("channel")) {
        auto channel_j = json["channel"];

        if (channel_j.isNumeric())
            commondev->set_rtlchannel(int_to_string(channel_j.asInt()));
        else if (channel_j.isString())
            commondev->set_rtlchannel(munge_to_printable(channel_j.asString()));
    }

    if (is_powermeter(json))
        add_powermeter(json, rtlholder);

    if (newrtl && commondev != NULL) {
        std::string info = "Detected new RTLAMR RF device '" + commondev->get_model() + "'";

        if (commondev->get_rtlid() != "") 
            info += " ID " + commondev->get_rtlid();

        if (commondev->get_rtlchannel() != "0")
            info += " Channel " + commondev->get_rtlchannel();

        _MSG(info, MSGFLAG_INFO);
    }

    return true;
}

bool Kis_RTLAMR_Phy::is_powermeter(Json::Value json) {

    auto msgjson = json["Message"];
    auto id_j = msgjson["ID"];
    auto consumption_j = msgjson["Consumption"];

    if (!id_j.isNull() || !consumption_j.isNull()) {
        return true;
    }

    return false;
}

void Kis_RTLAMR_Phy::add_powermeter(Json::Value json, std::shared_ptr<tracker_element_map> rtlholder) {
    auto msgjson = json["Message"];
    auto id_j = msgjson["ID"];
    auto consumption_j = msgjson["Consumption"];

    if (!id_j.isNull() || !consumption_j.isNull()) {
        auto powermeterdev = 
            rtlholder->get_sub_as<rtlamr_tracked_powermeter>(rtlamr_powermeter_id);

        if (powermeterdev == NULL) {
            powermeterdev = 
                std::make_shared<rtlamr_tracked_powermeter>(rtlamr_powermeter_id);
            rtlholder->insert(powermeterdev);
        }

        if (id_j.isNumeric()) {
            powermeterdev->set_id(id_j.asDouble());
        }

        if (consumption_j.isNumeric()) {
            powermeterdev->set_consumption(consumption_j.asDouble());
        }

    }
}



int Kis_RTLAMR_Phy::PacketHandler(CHAINCALL_PARMS) {
    Kis_RTLAMR_Phy *rtlamr = (Kis_RTLAMR_Phy *) auxdata;

    if (in_pack->error || in_pack->filtered || in_pack->duplicate)
        return 0;

    kis_json_packinfo *json = in_pack->fetch<kis_json_packinfo>(rtlamr->pack_comp_json);
    if (json == NULL)
        return 0;

    if (json->type != "RTLamr")
        return 0;

    std::stringstream ss(json->json_string);
    Json::Value device_json;

    try {
        ss >> device_json;

        if (rtlamr->json_to_rtl(device_json, in_pack)) {
            packet_metablob *metablob = in_pack->fetch<packet_metablob>(rtlamr->pack_comp_meta);
            if (metablob == NULL) {
                metablob = new packet_metablob("RTLAMR", json->json_string);
                in_pack->insert(rtlamr->pack_comp_meta, metablob);
            }
        }
    } catch (std::exception& e) {
        fprintf(stderr, "debug - error processing rtl json %s\n", e.what());
        return 0;
    }

    return 1;
}

