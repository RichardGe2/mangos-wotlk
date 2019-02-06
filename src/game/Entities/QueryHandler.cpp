/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Tools/Language.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"
#include "WorldPacket.h"
#include "Server/WorldSession.h"
#include "Server/Opcodes.h"
#include "Log.h"
#include "World/World.h"
#include "Globals/ObjectMgr.h"
#include "Entities/ObjectGuid.h"
#include "Entities/Player.h"
#include "Entities/NPCHandler.h"
#include "Server/SQLStorages.h"

void WorldSession::SendNameQueryOpcode(Player* p) const
{
    if (!p)
        return;
    // guess size
    WorldPacket data(SMSG_NAME_QUERY_RESPONSE, (8 + 1 + 1 + 1 + 1 + 1 + 10));
    data << p->GetPackGUID();                               // player guid
    data << uint8(0);                                       // added in 3.1; if > 1, then end of packet
    data << p->GetName();                                   // played name
    data << uint8(0);                                       // realm name for cross realm BG usage
    data << uint8(p->getRace());
    data << uint8(p->getGender());
    data << uint8(p->getClass());
    if (DeclinedName const* names = p->GetDeclinedNames())
    {
        data << uint8(1);                                   // is declined
        for (const auto& i : names->name)
            data << i;
    }
    else
        data << uint8(0);                                   // is not declined

    SendPacket(data);
}

void WorldSession::SendNameQueryOpcodeFromDB(ObjectGuid guid) const
{
    CharacterDatabase.AsyncPQuery(&WorldSession::SendNameQueryOpcodeFromDBCallBack, GetAccountId(),
                                  !sWorld.getConfig(CONFIG_BOOL_DECLINED_NAMES_USED) ?
                                  //   ------- Query Without Declined Names --------
                                  //          0     1     2     3       4
                                  "SELECT guid, name, race, gender, class "
                                  "FROM characters WHERE guid = '%u'"
                                  :
                                  //   --------- Query With Declined Names ---------
                                  //          0                1     2     3       4
                                  "SELECT characters.guid, name, race, gender, class, "
                                  //   5         6       7           8             9
                                  "genitive, dative, accusative, instrumental, prepositional "
                                  "FROM characters LEFT JOIN character_declinedname ON characters.guid = character_declinedname.guid WHERE characters.guid = '%u'",
                                  guid.GetCounter());
}

void WorldSession::SendNameQueryOpcodeFromDBCallBack(QueryResult* result, uint32 accountId)
{
    if (!result)
        return;

    WorldSession* session = sWorld.FindSession(accountId);
    if (!session)
    {
        delete result;
        return;
    }

    Field* fields = result->Fetch();
    uint32 lowguid      = fields[0].GetUInt32();
    std::string name = fields[1].GetCppString();
    uint8 pRace = 0, pGender = 0, pClass = 0;
    if (name.empty())
        name         = session->GetMangosString(LANG_NON_EXIST_CHARACTER);
    else
    {
        pRace        = fields[2].GetUInt8();
        pGender      = fields[3].GetUInt8();
        pClass       = fields[4].GetUInt8();
    }
    // guess size
    WorldPacket data(SMSG_NAME_QUERY_RESPONSE, (8 + 1 + 1 + 1 + 1 + 1 + 1 + 10));
    data << ObjectGuid(HIGHGUID_PLAYER, lowguid).WriteAsPacked();
    data << uint8(0);                                       // added in 3.1; if > 1, then end of packet
    data << name;
    data << uint8(0);                                       // realm name for cross realm BG usage
    data << uint8(pRace);                                   // race
    data << uint8(pGender);                                 // gender
    data << uint8(pClass);                                  // class

    // if the first declined name field (5) is empty, the rest must be too
    if (sWorld.getConfig(CONFIG_BOOL_DECLINED_NAMES_USED) && !fields[5].GetCppString().empty())
    {
        data << uint8(1);                                   // is declined
        for (int i = 5; i < MAX_DECLINED_NAME_CASES + 5; ++i)
            data << fields[i].GetCppString();
    }
    else
        data << uint8(0);                                   // is not declined

    session->SendPacket(data);
    delete result;
}

void WorldSession::HandleNameQueryOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;

    recv_data >> guid;

    Player* pChar = sObjectMgr.GetPlayer(guid);

    if (pChar)
        SendNameQueryOpcode(pChar);
    else
        SendNameQueryOpcodeFromDB(guid);
}

void WorldSession::HandleQueryTimeOpcode(WorldPacket& /*recv_data*/)
{
    SendQueryTimeResponse();
}

/// Only _static_ data send in this packet !!!
void WorldSession::HandleCreatureQueryOpcode(WorldPacket& recv_data)
{
    uint32 entry;
    recv_data >> entry;
    ObjectGuid guid;
    recv_data >> guid;

    CreatureInfo const* ci = ObjectMgr::GetCreatureTemplate(entry);
    if (ci)
    {
        int loc_idx = GetSessionDbLocaleIndex();

        char const* name = ci->Name;
        char const* subName = ci->SubName;
        sObjectMgr.GetCreatureLocaleStrings(entry, loc_idx, &name, &subName);

        DETAIL_LOG("WORLD: CMSG_CREATURE_QUERY '%s' - Entry: %u.", ci->Name, entry);
        // guess size
        WorldPacket data(SMSG_CREATURE_QUERY_RESPONSE, 100);
        data << uint32(entry);                              // creature entry
        data << name;
        data << uint8(0) << uint8(0) << uint8(0);           // name2, name3, name4, always empty
        data << subName;
        data << ci->IconName;                               // "Directions" for guard, string for Icons 2.3.0
        data << uint32(ci->CreatureTypeFlags);              // flags
        data << uint32(ci->CreatureType);                   // CreatureType.dbc
        data << uint32(ci->Family);                         // CreatureFamily.dbc
        data << uint32(ci->Rank);                           // Creature Rank (elite, boss, etc)
        data << uint32(ci->KillCredit[0]);                  // new in 3.1, kill credit
        data << uint32(ci->KillCredit[1]);                  // new in 3.1, kill credit

        for (unsigned int i : ci->ModelId)
            data << uint32(i);

        data << float(ci->HealthMultiplier);                // health multiplier
        data << float(ci->PowerMultiplier);                 // power multiplier
        data << uint8(ci->RacialLeader);
        for (unsigned int QuestItem : ci->QuestItems)
            data << uint32(QuestItem);              // itemId[6], quest drop
        data << uint32(ci->MovementTemplateId);             // CreatureMovementInfo.dbc
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_CREATURE_QUERY_RESPONSE");
    }
    else
    {
        DEBUG_LOG("WORLD: CMSG_CREATURE_QUERY - Guid: %s Entry: %u NO CREATURE INFO!",
                  guid.GetString().c_str(), entry);
        WorldPacket data(SMSG_CREATURE_QUERY_RESPONSE, 4);
        data << uint32(entry | 0x80000000);
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_CREATURE_QUERY_RESPONSE");
    }
}

/// Only _static_ data send in this packet !!!
void WorldSession::HandleGameObjectQueryOpcode(WorldPacket& recv_data)
{
    uint32 entryID;
    recv_data >> entryID;
    ObjectGuid guid;
    recv_data >> guid;

    const GameObjectInfo* info = ObjectMgr::GetGameObjectInfo(entryID);
    if (info)
    {
        std::string Name = info->name;
        std::string IconName = info->IconName;
        std::string CastBarCaption = info->castBarCaption;

        int loc_idx = GetSessionDbLocaleIndex();
        if (loc_idx >= 0)
        {
            GameObjectLocale const* gl = sObjectMgr.GetGameObjectLocale(entryID);
            if (gl)
            {
                if (gl->Name.size() > size_t(loc_idx) && !gl->Name[loc_idx].empty())
                    Name = gl->Name[loc_idx];
                if (gl->CastBarCaption.size() > size_t(loc_idx) && !gl->CastBarCaption[loc_idx].empty())
                    CastBarCaption = gl->CastBarCaption[loc_idx];
            }
        }
        DETAIL_LOG("WORLD: CMSG_GAMEOBJECT_QUERY '%s' - Entry: %u. ", info->name, entryID);
        WorldPacket data(SMSG_GAMEOBJECT_QUERY_RESPONSE, 150);
        data << uint32(entryID);
        data << uint32(info->type);
        data << uint32(info->displayId);
        data << Name;
        data << uint8(0) << uint8(0) << uint8(0);           // name2, name3, name4
        data << IconName;                                   // 2.0.3, string. Icon name to use instead of default icon for go's (ex: "Attack" makes sword)
        data << CastBarCaption;                             // 2.0.3, string. Text will appear in Cast Bar when using GO (ex: "Collecting")
        data << info->unk1;                                 // 2.0.3, string
        data.append(info->raw.data, 24);
        data << float(info->size);                          // go size
        for (unsigned int questItem : info->questItems)
            data << uint32(questItem);            // itemId[6], quest drop
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_GAMEOBJECT_QUERY_RESPONSE");
    }
    else
    {
        DEBUG_LOG("WORLD: CMSG_GAMEOBJECT_QUERY - Guid: %s Entry: %u Missing gameobject info!",
                  guid.GetString().c_str(), entryID);
        WorldPacket data(SMSG_GAMEOBJECT_QUERY_RESPONSE, 4);
        data << uint32(entryID | 0x80000000);
        SendPacket(data);
        DEBUG_LOG("WORLD: Sent SMSG_GAMEOBJECT_QUERY_RESPONSE");
    }
}

void WorldSession::HandleCorpseQueryOpcode(WorldPacket& /*recv_data*/)
{
    DETAIL_LOG("WORLD: Received opcode MSG_CORPSE_QUERY");

    Corpse* corpse = GetPlayer()->GetCorpse();

    if (!corpse)
    {
        WorldPacket data(MSG_CORPSE_QUERY, 1);
        data << uint8(0);                                   // corpse not found
        SendPacket(data);
        return;
    }

    uint32 corpsemapid = corpse->GetMapId();
    float x = corpse->GetPositionX();
    float y = corpse->GetPositionY();
    float z = corpse->GetPositionZ();
    int32 mapid = corpsemapid;

    // if corpse at different map
    if (corpsemapid != _player->GetMapId())
    {
        // search entrance map for proper show entrance
        if (MapEntry const* corpseMapEntry = sMapStore.LookupEntry(corpsemapid))
        {
            if (corpseMapEntry->IsDungeon() && corpseMapEntry->ghost_entrance_map >= 0)
            {
                // if corpse map have entrance
                if (TerrainInfo const* entranceMap = sTerrainMgr.LoadTerrain(corpseMapEntry->ghost_entrance_map))
                {
                    mapid = corpseMapEntry->ghost_entrance_map;
                    x = corpseMapEntry->ghost_entrance_x;
                    y = corpseMapEntry->ghost_entrance_y;
                    z = entranceMap->GetHeightStatic(x, y, MAX_HEIGHT);
                }
            }
        }
    }

    WorldPacket data(MSG_CORPSE_QUERY, 1 + (6 * 4));
    data << uint8(1);                                       // corpse found
    data << int32(mapid);
    data << float(x);
    data << float(y);
    data << float(z);
    data << uint32(corpsemapid);
    data << uint32(0);                                      // unknown
    SendPacket(data);
}

void WorldSession::HandleNpcTextQueryOpcode(WorldPacket& recv_data)
{
    uint32 textID;
    ObjectGuid guid;

    recv_data >> textID;
    recv_data >> guid;

    DETAIL_LOG("WORLD: CMSG_NPC_TEXT_QUERY ID '%u'", textID);

    GossipText const* pGossip = sObjectMgr.GetGossipText(textID);

    WorldPacket data(SMSG_NPC_TEXT_UPDATE, 100);            // guess size
    data << textID;

    if (!pGossip)
    {
        for (uint32 i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
        {
            data << float(0);
            data << "Greetings $N";
            data << "Greetings $N";
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
        }
    }
    else
    {
        std::string Text_0[MAX_GOSSIP_TEXT_OPTIONS], Text_1[MAX_GOSSIP_TEXT_OPTIONS];
        for (int i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
        {
            Text_0[i] = pGossip->Options[i].Text_0;
            Text_1[i] = pGossip->Options[i].Text_1;
        }

        int loc_idx = GetSessionDbLocaleIndex();

        sObjectMgr.GetNpcTextLocaleStringsAll(textID, loc_idx, &Text_0, &Text_1);

        for (int i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
        {
            data << pGossip->Options[i].Probability;

            if (Text_0[i].empty())
                data << Text_1[i];
            else
                data << Text_0[i];

            if (Text_1[i].empty())
                data << Text_0[i];
            else
                data << Text_1[i];

            data << pGossip->Options[i].Language;

            for (auto Emote : pGossip->Options[i].Emotes)
            {
                data << Emote._Delay;
                data << Emote._Emote;
            }
        }
    }

    SendPacket(data);

    DEBUG_LOG("WORLD: Sent SMSG_NPC_TEXT_UPDATE");
}

void WorldSession::HandlePageTextQueryOpcode(WorldPacket& recv_data)
{
    DETAIL_LOG("WORLD: Received opcode CMSG_PAGE_TEXT_QUERY");
    recv_data.hexlike();

    uint32 pageID;
    recv_data >> pageID;
    recv_data.read_skip<uint64>();                          // guid















	

	/////////////////////////////////////////////
	// RICHARD : decouverte d'un livre

	//a noter que  HandlePageTextQueryOpcode  est appelé QUE si le client n'a pas la page dans ses fichier de cache
	//d'ou l'important ce bien cleaner le cache de client  (dossier WDB)

	//il faudra peut etre que je definisse mieux qu'est ce qu'un texte a decouvrir et s'il y a des textes qu'on prends pas en compte dans l'achievement

	//commane pour voir les gameobject qui ont du texte :
	//SELECT entry,NAME,data0 FROM gameobject_template WHERE TYPE=9 AND data0>0
	//il y en a 108
	//
	//j'avais oublié les objets type GAMEOBJECT_TYPE_GOOBER
	//la bonne commande est donc :
	//SELECT entry,NAME,data0,data7 FROM gameobject_template WHERE TYPE=9 AND data0>0 OR TYPE=10 AND data7>0
	//il y en a 111

	if ( pageID )
	{

		//on chercher si  pageID  est dans  m_richa_pageDiscovered
		bool existInDataBase_ofThisPerso = false;
		bool comesFromObject = false;
		for(int i=0; i<_player->m_richa_pageDiscovered.size(); i++)
		{
			if ( _player->m_richa_pageDiscovered[i].pageId == pageID )
			{
				existInDataBase_ofThisPerso = true;
				if ( _player->m_richa_pageDiscovered[i].objectID != 0 )
				{
					comesFromObject = true;
				}
				break;
			}
		}

		if ( !existInDataBase_ofThisPerso )
		{
			int objectId = 0;  // correspond a   object=XXX  dans  vanillagaming
			int itemId = 0; // correspond a item=xxx

			char command4[2048];
			sprintf(command4,"SELECT entry FROM gameobject_template WHERE TYPE=9 AND data0=%d",pageID);
			if (QueryResult* result4 = WorldDatabase.PQuery( command4 ))
			{
				if ( result4->GetRowCount() != 1 )
				{
					BASIC_LOG("ERROR 8453");
					_player->Say("ERROR : OBJECT NON TROUVE 001", LANG_UNIVERSAL);
				}

				BarGoLink bar4(result4->GetRowCount());
				bar4.step();
				Field* fields = result4->Fetch();
				objectId = fields->GetInt32();
				delete result4;

				comesFromObject = true;
			}
			else
			{
				//si on arrive la, c'est peut etre un item , example d'item :  5088  -  Control Console Operating Manual
				//note : les textes venant d'un  item ne font pas parti du succes de tout lire
				//       mais au cas ou, on va les sauvegarder quand meme - ca coute rien

				char command5[2048];
				sprintf(command5,"SELECT entry FROM item_template WHERE PageText=%d",pageID);
				if (QueryResult* result5 = WorldDatabase.PQuery( command5 ))
				{
					if ( result5->GetRowCount() != 1 )
					{
						BASIC_LOG("ERROR 8453");
						_player->Say("ERROR : OBJECT NON TROUVE 003", LANG_UNIVERSAL);
					}

					BarGoLink bar5(result5->GetRowCount());
					bar5.step();
					Field* fields = result5->Fetch();
					itemId = fields->GetInt32();
					delete result5;
				}
				else
				{
					//si on arrive ici, le text est ni dans un gameobject ni dans un item... a étudier...

					BASIC_LOG("ERROR 8454  ----  pageID = %d", pageID);
					_player->Say("ERROR : OBJECT NON TROUVE 002", LANG_UNIVERSAL);
				}
			

			}



			//si le livre n'est pas connu par ce perso, on l'ajoute a la liste de ce perso
			// a noter qu'on fait ca, MEME si un autre perso assicié connait deja ce livre
			_player->m_richa_pageDiscovered.push_back( Player::RICHA_PAGE_DISCO_STAT( pageID , objectId, itemId) ); 



			// par curiosite, on regarde si un autre perso du meme joueur humain connait ce texte

			//bool knownByOtherPerso = false;
			std::vector<std::string> listePersoQuiConnaissentDeja;

			std::vector<int>  associatedPlayerGUID;

			// #LISTE_ACCOUNT_HERE   -  ce hashtag repere tous les endroit que je dois updater quand je rajoute un nouveau compte - ou perso important
			if ( _player->GetGUIDLow() == 4 )// boulette
			{
				associatedPlayerGUID.push_back(27); // Bouzigouloum
			}
			if ( _player->GetGUIDLow() == 27 )//  Bouzigouloum 
			{
				associatedPlayerGUID.push_back(4); // boulette
			}
			if ( _player->GetGUIDLow() == 5 )// Bouillot
			{
				associatedPlayerGUID.push_back(28); // Adibou
			}
			if ( _player->GetGUIDLow() == 28 )// Adibou 
			{
				associatedPlayerGUID.push_back(5); //  Bouillot
			}

			//juste pour le debug je vais lier grandjuge et grandtroll
			if ( _player->GetGUIDLow() == 19 )// grandjuge
			{
				associatedPlayerGUID.push_back(29); // grandtroll
			}
			if ( _player->GetGUIDLow() == 29 )// grandtroll 
			{
				associatedPlayerGUID.push_back(19); //  grandjuge
			}


			for(int i=0; i<associatedPlayerGUID.size(); i++)
			{
				std::vector<Player::RICHA_NPC_KILLED_STAT> richa_NpcKilled;
				std::vector<Player::RICHA_PAGE_DISCO_STAT> richa_pageDiscovered;
				std::vector<Player::RICHA_LUNARFESTIVAL_ELDERFOUND> richa_lunerFestivalElderFound;
				std::vector<Player::RICHA_MAISON_TAVERN> richa_maisontavern;
				std::vector<Player::RICHA_ITEM_LOOT_QUEST> richa_lootquest;
				std::string persoName;
				Player::richa_importFrom_richaracter_(
					associatedPlayerGUID[i],
					richa_NpcKilled,
					richa_pageDiscovered,
					richa_lunerFestivalElderFound,
					richa_maisontavern,
					richa_lootquest,
					persoName
					);

				for(int j=0; j<richa_pageDiscovered.size(); j++)
				{
					if ( richa_pageDiscovered[j].pageId == pageID )
					{
						listePersoQuiConnaissentDeja.push_back(persoName);
						//knownByOtherPerso = true;
						break;
					}
				}//pour chaque page connu du perso associe


				//if ( knownByOtherPerso )
				{
				//	break; // on break pas car on va remplir la liste de tous les perso qui connaissent
				}


			}//pour chaque perso associé



			if ( listePersoQuiConnaissentDeja.size() == 0 )
			{
				if ( comesFromObject ) // dans le succes, on compte QUE les 111 textes qui viennet d'un object et PAS d'un item
				{
					char messageOut[2048];
					sprintf(messageOut, "Decouverte d'un nouveau texte!");
					_player->Say(messageOut, LANG_UNIVERSAL);
				}
			}
			else
			{
				if ( comesFromObject ) // dans le succes, on compte QUE les 111 textes qui viennet d'un object et PAS d'un item
				{
					char messageOut[2048];
					sprintf(messageOut, "Texte deja connu par %d autre Perso: ",listePersoQuiConnaissentDeja.size());
					for(int i=0; i<listePersoQuiConnaissentDeja.size(); i++)
					{
						strcat(messageOut,listePersoQuiConnaissentDeja[i].c_str());
						if ( i != listePersoQuiConnaissentDeja.size()-1 ) { strcat(messageOut,", "); }
					}
					_player->Say(messageOut, LANG_UNIVERSAL);
				}
			}

		
		}
		else
		{
			if ( comesFromObject ) // dans le succes, on compte QUE les 111 textes qui viennet d'un object et PAS d'un item
			{
				char messageOut[4096];
				sprintf(messageOut, "Texte deja connu par ce Perso.");
				_player->Say(messageOut, LANG_UNIVERSAL);
			}
		}

	} // if pageId
	//////////////////////////////////////////


















    while (pageID)
    {
        PageText const* pPage = sPageTextStore.LookupEntry<PageText>(pageID);
        // guess size
        WorldPacket data(SMSG_PAGE_TEXT_QUERY_RESPONSE, 50);
        data << pageID;

        if (!pPage)
        {
            data << "Item page missing.";
            data << uint32(0);
            pageID = 0;
        }
        else
        {
            std::string Text = pPage->Text;

            int loc_idx = GetSessionDbLocaleIndex();
            if (loc_idx >= 0)
            {
                PageTextLocale const* pl = sObjectMgr.GetPageTextLocale(pageID);
                if (pl)
                {
                    if (pl->Text.size() > size_t(loc_idx) && !pl->Text[loc_idx].empty())
                        Text = pl->Text[loc_idx];
                }
            }

            data << Text;
            data << uint32(pPage->Next_Page);
            pageID = pPage->Next_Page;
        }
        SendPacket(data);

        DEBUG_LOG("WORLD: Sent SMSG_PAGE_TEXT_QUERY_RESPONSE");
    }
}

void WorldSession::HandleCorpseMapPositionQueryOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Recv CMSG_CORPSE_MAP_POSITION_QUERY");

    uint32 unk;
    recv_data >> unk;

    WorldPacket data(SMSG_CORPSE_TRANSPORT_QUERY, 4 + 4 + 4 + 4);
    data << float(0);
    data << float(0);
    data << float(0);
    data << float(0);
    SendPacket(data);
}

void WorldSession::HandleQueryQuestsCompletedOpcode(WorldPacket& /*recv_data */)
{
    uint32 count = 0;

    WorldPacket data(SMSG_ALL_QUESTS_COMPLETED, 4 + 4 * count);
    data << uint32(count);

    for (QuestStatusMap::const_iterator itr = _player->getQuestStatusMap().begin(); itr != _player->getQuestStatusMap().end(); ++itr)
    {
        if (itr->second.m_rewarded)
        {
            data << uint32(itr->first);
            ++count;
        }
    }
    data.put<uint32>(0, count);
    SendPacket(data);
}

void WorldSession::HandleQuestPOIQueryOpcode(WorldPacket& recv_data)
{
    uint32 count;
    recv_data >> count;                                     // quest count, max=25

    if (count > MAX_QUEST_LOG_SIZE)
    {
        recv_data.rpos(recv_data.wpos());                   // set to end to avoid warnings spam
        return;
    }

    WorldPacket data(SMSG_QUEST_POI_QUERY_RESPONSE, 4 + (4 + 4)*count);
    data << uint32(count);                                  // count

    for (uint32 i = 0; i < count; ++i)
    {
        uint32 questId;
        recv_data >> questId;                               // quest id

        bool questOk = false;

        uint16 questSlot = _player->FindQuestSlot(questId);

        if (questSlot != MAX_QUEST_LOG_SIZE)
            questOk = _player->GetQuestSlotQuestId(questSlot) == questId;

        if (questOk)
        {
            QuestPOIVector const* POI = sObjectMgr.GetQuestPOIVector(questId);

            if (POI)
            {
                data << uint32(questId);                    // quest ID
                data << uint32(POI->size());                // POI count

                for (const auto& itr : *POI)
                {
                    data << uint32(itr.PoiId);             // POI index
                    data << int32(itr.ObjectiveIndex);     // objective index
                    data << uint32(itr.MapId);             // mapid
                    data << uint32(itr.MapAreaId);         // world map area id
                    data << uint32(itr.FloorId);           // floor id
                    data << uint32(itr.Unk3);              // unknown
                    data << uint32(itr.Unk4);              // unknown
                    data << uint32(itr.points.size());     // POI points count

                    for (std::vector<QuestPOIPoint>::const_iterator itr2 = itr.points.begin(); itr2 != itr.points.end(); ++itr2)
                    {
                        data << int32(itr2->x);             // POI point x
                        data << int32(itr2->y);             // POI point y
                    }
                }
            }
            else
            {
                data << uint32(questId);                    // quest ID
                data << uint32(0);                          // POI count
            }
        }
        else
        {
            data << uint32(questId);                        // quest ID
            data << uint32(0);                              // POI count
        }
    }

    SendPacket(data);
}

void WorldSession::SendQueryTimeResponse() const
{
    WorldPacket data(SMSG_QUERY_TIME_RESPONSE, 4 + 4);
    data << uint32(time(nullptr));
    data << uint32(sWorld.GetNextDailyQuestsResetTime() - time(nullptr));
    SendPacket(data);
}
