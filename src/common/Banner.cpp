/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Banner.h"
#include "GitRevision.h"
#include "StringFormat.h"

std::string FormatBuildDateTimeBaner(const char* dateStr, const char* timeStr)
{
    std::unordered_map<std::string, std::string> monthMap = {
        {"Jan", "1"}, {"Feb", "2"}, {"Mar", "3"}, {"Apr", "4"},
        {"May", "5"}, {"Jun", "6"}, {"Jul", "7"}, {"Aug", "8"},
        {"Sep", "9"}, {"Oct", "10"}, {"Nov", "11"}, {"Dec", "12"}
    };

    // 解析日期
    std::istringstream dateStream(dateStr);
    std::string mon, day, year;
    dateStream >> mon >> day >> year;

    // 去除前导0
    if (day[0] == '0')
        day = day.substr(1);

    // 解析时间
    std::istringstream timeStream(timeStr);
    std::string hour, minute, second;
    std::getline(timeStream, hour, ':');
    std::getline(timeStream, minute, ':');
    std::getline(timeStream, second, ':');

    return year + "年" + monthMap[mon] + "月" + day + "日 " +
        hour + "时" + minute + "分" + second + "秒";
}


void Acore::Banner::Show(std::string_view applicationName, void(*log)(std::string_view text), void(*logExtraInfo)())
{
    log(Acore::StringFormat("{} ({})", GitRevision::GetFullVersion(), applicationName));
    log("<Ctrl-C> to stop.\n");
    log("██╗     ██╗    ██╗ ██████╗ ██████╗ ██████╗ ███████╗ ");
    log("██║     ██║    ██║██╔════╝██╔═══██╗██╔══██╗██╔════╝ ");
    log("██║     ██║ █╗ ██║██║     ██║   ██║██████╔╝█████╗   ");
    log("██║     ██║███╗██║██║     ██║   ██║██╔══██╗██╔══╝   ");
    log("███████╗╚███╔███╔╝╚██████╗╚██████╔╝██║  ██║███████╗ ");
    log("╚══════╝ ╚══╝╚══╝  ╚═════╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝ \n");
    log(" Based on AzerothCore 3.3.5a  -  www.azerothcore.org\n");

    log("'########::'##::::::::::'###::::'##:::'##:'########:'########::'########:::'#######::'########::'######::");
    log(" ##.... ##: ##:::::::::'## ##:::. ##:'##:: ##.....:: ##.... ##: ##.... ##:'##.... ##:... ##..::'##... ##:");
    log(" ##:::: ##: ##::::::::'##:. ##:::. ####::: ##::::::: ##:::: ##: ##:::: ##: ##:::: ##:::: ##:::: ##:::..::");
    log(" ########:: ##:::::::'##:::. ##:::. ##:::: ######::: ########:: ########:: ##:::: ##:::: ##::::. ######::");
    log(" ##.....::: ##::::::: #########:::: ##:::: ##...:::: ##.. ##::: ##.... ##: ##:::: ##:::: ##:::::..... ##:");
    log(" ##:::::::: ##::::::: ##.... ##:::: ##:::: ##::::::: ##::. ##:: ##:::: ##: ##:::: ##:::: ##::::'##::: ##:");
    log(" ##:::::::: ########: ##:::: ##:::: ##:::: ########: ##:::. ##: ########::. #######::::: ##::::. ######::");
    log("..:::::::::........::..:::::..:::::..:::::........::..:::::..::........::::.......::::::..::::::......:::\n");
    log("巫妖王之怒PLAYERBOTS(基于liyunfan PB) 仿官版本. 编译时间: " + FormatBuildDateTimeBaner(__DATE__, __TIME__) + "\n");


    if (logExtraInfo)
    {
        logExtraInfo();
    }

    log(" ");
}
