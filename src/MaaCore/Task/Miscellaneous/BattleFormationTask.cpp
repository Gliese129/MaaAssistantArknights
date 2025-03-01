#include "BattleFormationTask.h"

#include "Utils/Ranges.hpp"

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/Miscellaneous/CharSkillConfig.h"
#include "Config/Miscellaneous/CopilotConfig.h"
#include "Config/Miscellaneous/SSSCopilotConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/ImageIo.hpp"
#include "Utils/Logger.hpp"
#include "Vision/MultiMatcher.h"
#include "Vision/TemplDetOCRer.h"

#include <numeric>

void asst::BattleFormationTask::append_additional_formation(AdditionalFormation formation)
{
    m_additional.emplace_back(std::move(formation));
}

void asst::BattleFormationTask::set_support_unit_name(std::string upport_unit_name)
{
    m_support_unit_name = upport_unit_name;
}


void asst::BattleFormationTask::set_support_unit(std::pair<battle::Role, OperGroup> support_unit)
{
    m_support_unit = std::move(support_unit);
}

void asst::BattleFormationTask::set_add_user_additional(bool add_user_additional)
{
    m_add_user_additional = add_user_additional;
}

void asst::BattleFormationTask::set_user_additional(std::vector<std::pair<std::string, int>> user_additional)
{
    m_user_additional = std::move(user_additional);
}

void asst::BattleFormationTask::set_add_trust(bool add_trust)
{
    m_add_trust = add_trust;
}

void asst::BattleFormationTask::set_use_support(bool use_support) {
    m_use_support = use_support;
}


void asst::BattleFormationTask::set_select_formation(int index)
{
    m_select_formation_index = index;
}

void asst::BattleFormationTask::set_data_resource(DataResource resource)
{
    m_data_resource = resource;
}

bool asst::BattleFormationTask::_run()
{
    LogTraceFunction;

    if (!parse_formation()) {
        return false;
    }

    if (m_select_formation_index > 0 && !select_formation(m_select_formation_index)) {
        return false;
    }

    if (!enter_selection_page()) {
        save_img(utils::path("debug") / utils::path("other"));
        return false;
    }

    std::unordered_map<battle::Role, std::vector<OperGroup>> unselected_opers;
    unselected_opers.clear();

    for (auto& [role, oper_groups] : m_formation) {
        //unselected_opers[role] = add_formation(role, oper_groups); // TODO chage back
        unselected_opers[role] = std::move(oper_groups);
    }
        
    add_additional();

    if (m_add_user_additional) {
        for (const auto& [name, skill] : m_user_additional) {
            if (m_operators_in_formation.contains(name)) {
                continue;
            }
            if (m_size_of_operators_in_formation >= 12) {
                break;
            }
            asst::battle::OperUsage oper;
            oper.name = name;
            oper.skill = skill;
            std::vector<asst::battle::OperUsage> usage { std::move(oper) };
            m_user_formation.emplace_back(std::move(usage));
            ++m_size_of_operators_in_formation;
        }
        m_size_of_operators_in_formation -= (int)m_user_formation.size();
        add_formation(battle::Role::Unknown, m_user_formation);
    }

    add_additional();
    if (m_add_trust) {
        add_trust_operators();
    }
    confirm_selection();

    // 查找需要助战的干员
    std::pair<battle::Role, OperGroup> support = {};
    size_t cnt = 0;

    std::string unselected_groups = "";

    for (auto& [role, oper_groups] : unselected_opers) {
        cnt += oper_groups.size();
        if (oper_groups.size()) {
            set_support_unit({ role, oper_groups[0] });
        }
        for (auto& oper_group : oper_groups) {
            std::string name_list = "[";
            for (auto& oper_usage : oper_group) {
                name_list += oper_usage.name + ", ";
            }
            name_list = name_list.substr(0, name_list.length() - 2) + "]";
            unselected_groups += name_list + ", " + (oper_group.size() > 1 ? "\n" : "");
        }
    }
    if (unselected_groups.length()) {
        unselected_groups = unselected_groups.substr(0, unselected_groups.length() - 2);
    }

    if (cnt + !m_use_support > 1) {
        Log.error("BattleSupportTask: too many support operators");

        json::value info = basic_info_with_what("BattleTooManyUnselected");
        auto& details = info["details"];
        details["unselected_num"] = cnt;
        details["unselected"] = unselected_groups;
        callback(AsstMsg::SubTaskExtraInfo, info);
        _permanent_failed = true;
        return false;
    }

    // 使用助战
    if (m_use_support && cnt == 1) {
        if (!enter_support_page()) {
            return false;
        }
        click_support_role_table();
        while (!need_exit()) {
            if (select_support_oper()) {
                ProcessTask(*this, { "BattleSupportFormationConfirm" }).run();
                break;
            }
            // 刷新到助战出现
            while (!ProcessTask(*this, { "BattleSupportUnitRefresh" }).run()) {}
        }
    }

    return true;
}

std::vector<asst::BattleFormationTask::OperGroup> asst::BattleFormationTask::add_formation(
    battle::Role role, std::vector<OperGroup> oper_groups)
{
    click_role_table(role);
    bool has_error = false;
    int swipe_times = 0, error_times = 0;
    while (!need_exit()) {
        if (select_opers_in_cur_page(oper_groups)) {
            has_error = false;
            if (oper_groups.empty()) {
                break;
            }
            swipe_page();
            ++swipe_times;
        }
        else if (has_error) {
            swipe_to_the_left(swipe_times);
            // reset page
            click_role_table(role == battle::Role::Unknown ? battle::Role::Pioneer : battle::Role::Unknown);
            click_role_table(role);
            swipe_to_the_left(swipe_times);
            swipe_times = 0;
            has_error = false;
            error_times = 0;
        }
        else {
            has_error = true;
            swipe_to_the_left(swipe_times);
            swipe_times = 0;
            error_times++;
        }
        if (error_times >= 2) {
            // for a operator, if he/she's not found in 2 rows, he/she should be not in the box 
            break;
        }
    }
    return std::move(oper_groups);
}

bool asst::BattleFormationTask::add_additional()
{
    // （但是干员名在除开获取时间的情况下都会被遮挡，so ?
    LogTraceFunction;

    if (m_additional.empty()) {
        return false;
    }

    for (const auto& addition : m_additional) {
        std::string filter_name;
        switch (addition.filter) {
        case Filter::Cost:
            filter_name = "BattleQuickFormationFilter-Cost";
            break;
        case Filter::Trust:
            // TODO
            break;
        default:
            break;
        }
        if (!filter_name.empty()) {
            ProcessTask(*this, { "BattleQuickFormationFilter" }).run();
            ProcessTask(*this, { filter_name }).run();
            if (addition.double_click_filter) {
                ProcessTask(*this, { filter_name }).run();
            }
            ProcessTask(*this, { "BattleQuickFormationFilterClose" }).run();
        }
        for (const auto& [role, number] : addition.role_counts) {
            // unknown role means "all"
            click_role_table(role);

            auto opers_result = analyzer_opers();

            // TODO 这里要识别一下干员之前有没有被选中过
            for (size_t i = 0; i < static_cast<size_t>(number) && i < opers_result.size(); ++i) {
                const auto& oper = opers_result.at(i);
                ctrler()->click(oper.rect);
            }
        }
    }

    return true;
}

bool asst::BattleFormationTask::add_trust_operators()
{
    LogTraceFunction;

    if (need_exit()) {
        return false;
    }

    // 需要追加的信赖干员数量
    int append_count = 12 - m_size_of_operators_in_formation;
    if (append_count == 0) {
        return true;
    }

    ProcessTask(*this, { "BattleQuickFormationFilter" }).run();
    // 双击信赖
    ProcessTask(*this, { "BattleQuickFormationFilter-Trust" }).run();
    ProcessTask(*this, { "BattleQuickFormationFilterClose" }).run();

    // 重置职业选择，保证处于最左
    click_role_table(battle::Role::Caster);
    click_role_table(battle::Role::Unknown);
    int failed_count = 0;
    while (!need_exit() && append_count > 0 && failed_count < 3) {
        MultiMatcher matcher(ctrler()->get_image());
        matcher.set_task_info("BattleQuickFormationTrustIcon");
        if (!matcher.analyze() || matcher.get_result().size() == 0) {
            failed_count++;
        }
        else {
            failed_count = 0;
            std::vector<MatchRect> result = matcher.get_result();
            // 按先上下后左右排个序
            sort_by_vertical_(result);
            for (const auto& trust_icon : result) {
                // 匹配完干员左下角信赖表，将roi偏移到整个干员标
                ctrler()->click(trust_icon.rect.move({ 20, -225, 110, 250 }));
                --append_count;
                if (append_count <= 0 || need_exit()) {
                    break;
                }
            }
        }
        if (!need_exit() && append_count > 0) {
            swipe_page();
        }
    }

    return append_count == 0;
}

bool asst::BattleFormationTask::select_random_support_unit()
{
    return ProcessTask(*this, { "BattleSupportUnitFormation" }).run() &&
           ProcessTask(*this, { "BattleSupportUnitFormationSelectRandom" }).run();
}

asst::TextRect asst::BattleFormationTask::analyzer_support_opers_skill() {
    auto formation_task_ptr = Task.get("BattleSupportUnitSkillOCR"); //
    auto image = ctrler()->get_image();
    const auto& ocr_replace = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
    std::vector<TemplDetOCRer::Result> skills_result;

    std::string task_name = "BattleSupportUnitSkill-DetailedInfo";

    TemplDetOCRer name_analyzer(image);
    name_analyzer.set_task_info(task_name, "BattleSupportUnitSkillOCR");
    name_analyzer.set_replace(ocr_replace->replace_map, ocr_replace->replace_full);
    auto cur_opt = name_analyzer.analyze();

    for (auto& res : *cur_opt) {
        constexpr int kMinDistance = 5;
        auto find_it = ranges::find_if(skills_result, [&res](const TemplDetOCRer::Result& pre) {
            return std::abs(pre.flag_rect.x - res.flag_rect.x) < kMinDistance &&
                std::abs(pre.flag_rect.y - res.flag_rect.y) < kMinDistance;
        });
        if (find_it != skills_result.end()) {
            continue;
        }
        skills_result.emplace_back(std::move(res));
    }

    if (skills_result.empty()) {
        Log.error("BattleSupportUnitSkillOCR: no skill found");
    } else if (skills_result.size() != 1) {
        Log.error("BattleSupportUnitSkillOCR: more than one skill found");
    }

    std::vector<TextRect> tr_res;
    for (const auto& res : skills_result) {
        tr_res.emplace_back(TextRect { res.rect, res.score, res.text });
    }
    Log.info(tr_res);


    return tr_res[0];
}


std::vector<asst::TextRect> asst::BattleFormationTask::analyzer_support_opers()
{
    auto formation_task_ptr = Task.get("BattleSupportFormationOCR");
    auto image = ctrler()->get_image();
    const auto& ocr_replace = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
    std::vector<TemplDetOCRer::Result> opers_result;

    std::string task_name = "BattleSupportFormation-DetailedInfoBase";

    TemplDetOCRer name_analyzer(image);
    name_analyzer.set_task_info(task_name, "BattleSupportFormationOCR");
    name_analyzer.set_replace(ocr_replace->replace_map, ocr_replace->replace_full);
    auto cur_opt = name_analyzer.analyze();

    for (auto& res : *cur_opt) {
        constexpr int kMinDistance = 5;
        auto find_it = ranges::find_if(opers_result, [&res](const TemplDetOCRer::Result& pre) {
            return std::abs(pre.flag_rect.x - res.flag_rect.x) < kMinDistance &&
                    std::abs(pre.flag_rect.y - res.flag_rect.y) < kMinDistance;
        });
        if (find_it != opers_result.end()) {
            continue;
        }
        opers_result.emplace_back(std::move(res));
    }

    if (opers_result.empty()) {
        Log.error("BattleSupportFormationOCR: no oper found");
        return {};
    }
    sort_by_vertical_(opers_result);

    std::vector<TextRect> tr_res;
    for (const auto& res : opers_result) {
        tr_res.emplace_back(TextRect { res.rect, res.score, res.text });
    }
    Log.info(tr_res);
    return tr_res;
}

std::vector<asst::TextRect> asst::BattleFormationTask::analyzer_opers()
{
    auto formation_task_ptr = Task.get("BattleQuickFormationOCR");
    auto image = ctrler()->get_image();
    const auto& ocr_replace = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
    std::vector<TemplDetOCRer::Result> opers_result;

    for (int i = 0; i < 8; ++i) {
        std::string task_name = "BattleQuickFormation-OperNameFlag" + std::to_string(i);

        const auto& params = Task.get("BattleQuickFormationOCR")->special_params;
        TemplDetOCRer name_analyzer(image);

        name_analyzer.set_task_info(task_name, "BattleQuickFormationOCR");
        name_analyzer.set_bin_threshold(params[0]);
        name_analyzer.set_bin_expansion(params[1]);
        name_analyzer.set_bin_trim_threshold(params[2], params[3]);
        name_analyzer.set_replace(ocr_replace->replace_map, ocr_replace->replace_full);
        auto cur_opt = name_analyzer.analyze();
        if (!cur_opt) {
            continue;
        }
        for (auto& res : *cur_opt) {
            constexpr int kMinDistance = 5;
            auto find_it = ranges::find_if(opers_result, [&res](const TemplDetOCRer::Result& pre) {
                return std::abs(pre.flag_rect.x - res.flag_rect.x) < kMinDistance &&
                       std::abs(pre.flag_rect.y - res.flag_rect.y) < kMinDistance;
            });
            if (find_it != opers_result.end() || res.text.empty()) {
                continue;
            }
            opers_result.emplace_back(std::move(res));
        }
    }

    if (opers_result.empty()) {
        Log.error("BattleFormationTask: no oper found");
        return {};
    }
    sort_by_vertical_(opers_result);

    std::vector<TextRect> tr_res;
    for (const auto& res : opers_result) {
        tr_res.emplace_back(TextRect { res.rect, res.score, res.text });
    }
    Log.info(tr_res);
    return tr_res;
}

bool asst::BattleFormationTask::enter_selection_page()
{
    return ProcessTask(*this, { "BattleQuickFormation" }).set_retry_times(3).run();
}

bool asst::BattleFormationTask::enter_support_page() 
{
    return ProcessTask(*this, { "BattleSupportUnitFormation" }).run();
}

bool asst::BattleFormationTask::select_support_oper()
{
    auto opers_result = analyzer_support_opers();
    auto& group = m_support_unit.second;
    std::string selected_unit_name = "";
    int selected_unit_skill = -1;

    // TODO 目前只能判断干员，不能判断技能
    int delay = Task.get("BattleSupportFormationOCR")->post_delay;
    for (const auto& res : opers_result) {
        const std::string& name = res.text;
        bool found = false;
        for (const auto& oper : group) {
            if (oper.name == name) {
                found = true;
                selected_unit_name = name;
                selected_unit_skill = oper.skill;
                break;
            }
        }

        if (found) {
            // assume the center of the name box is always at the fixed relative position
            int xCenter = res.rect.x + res.rect.width / 2, yCenter = res.rect.y + res.rect.height / 2;
            // <info button> - <center of the name box> = (+42, -286)
            int xClick = xCenter + 42, yClick = yCenter - 286;
            Rect clickRect = { xClick, yClick, 1, 1 };
            ctrler()->click(clickRect);
            sleep(delay);

            // swipe to the bottom to make sure the name box is fully visible
            swipe_support_info_page();

            // get the skill name of the support unit
            auto rect = analyzer_support_opers_skill();
            std::string skill_name = rect.text;
            auto demaned_skill_name = asst::CharSkill.get_skill_name_by_pos(selected_unit_name, selected_unit_skill);

            ProcessTask(*this, { "ReturnOnce" }).run();

            if (check_if_skill_name_match(skill_name, demaned_skill_name)) {
                ctrler()->click(res.rect);
                sleep(delay);

                json::value info = basic_info_with_what("BattleSupportSelected");
                auto& details = info["details"];
                details["selected"] = name;
                callback(AsstMsg::SubTaskExtraInfo, info);

                return true;
            }
        }
    }
 
    return false;
}

bool asst::BattleFormationTask::check_if_skill_name_match(std::string skill_name, std::string demaned_skill_name,
                                                        double threshold)
{
    /* TODO: Need to find a better way
    This function uses dp to calculate the longest common substring of two strings, and if ratio of the length of the longest common substring to the length of skill_name is greater than threshold, then return true.
    */
    auto n = skill_name.length(), m = demaned_skill_name.length();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    int max_len = 0;
    for (int i = 1; i <= n; ++i) {
        char c1 = skill_name[i - 1];
        for (int j = 1; j <= m; ++j) {
            char c2 = demaned_skill_name[j - 1];
            if (c1 == c2) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
                max_len = std::max(max_len, dp[i][j]);
            }
            else {
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
            }
        }
    }
    return (double)max_len / n >= threshold;
}
 
bool asst::BattleFormationTask::select_opers_in_cur_page(std::vector<OperGroup>& groups)
{
    auto opers_result = analyzer_opers();

    static const std::array<Rect, 3> SkillRectArray = {
        Task.get("BattleQuickFormationSkill1")->specific_rect,
        Task.get("BattleQuickFormationSkill2")->specific_rect,
        Task.get("BattleQuickFormationSkill3")->specific_rect,
    };

    if (!opers_result.empty()) {
        if (m_last_oper_name == opers_result.back().text) {
            Log.info("last oper name is same as current, skip");
            return false;
        }
        m_last_oper_name = opers_result.back().text;
    }

    int delay = Task.get("BattleQuickFormationOCR")->post_delay;
    int skill = 0;
    for (const auto& res : opers_result) {
        const std::string& name = res.text;
        bool found = false;
        auto iter = groups.begin();
        for (; iter != groups.end(); ++iter) {
            for (const auto& oper : *iter) {
                if (oper.name == name) {
                    found = true;
                    skill = oper.skill;

                    m_operators_in_formation.emplace(name);
                    ++m_size_of_operators_in_formation;
                    break;
                }
            }
            if (found) {
                break;
            }
        }

        if (iter == groups.end()) {
            continue;
        }

        ctrler()->click(res.rect);
        sleep(delay);
        if (1 <= skill && skill <= 3) {
            if (skill == 3) {
                ProcessTask(*this, { "BattleQuickFormationSkill-SwipeToTheDown" }).run();
            }
            ctrler()->click(SkillRectArray.at(skill - 1ULL));
            sleep(delay);
        }
        groups.erase(iter);

        json::value info = basic_info_with_what("BattleFormationSelected");
        auto& details = info["details"];
        details["selected"] = name;
        callback(AsstMsg::SubTaskExtraInfo, info);
    }

    return true;
}

void asst::BattleFormationTask::swipe_page()
{
    ProcessTask(*this, { "BattleFormationOperListSlowlySwipeToTheRight" }).run();
}

void asst::BattleFormationTask::swipe_support_info_page() {
    ProcessTask(*this, { "BattleSupportUnitFormationOperInfoSwipeToTheDown" }).run();
}

void asst::BattleFormationTask::swipe_to_the_left(int times)
{
    for (int i = 0; i < times; ++i) {
        ProcessTask(*this, { "BattleFormationOperListSwipeToTheLeft" }).run();
    }
}



bool asst::BattleFormationTask::confirm_selection()
{
    return ProcessTask(*this, { "BattleQuickFormationConfirm" }).run();
}

bool asst::BattleFormationTask::click_role_table(battle::Role role)
{
    static const std::unordered_map<battle::Role, std::string> RoleNameType = {
        { battle::Role::Caster, "Caster" }, { battle::Role::Medic, "Medic" },     { battle::Role::Pioneer, "Pioneer" },
        { battle::Role::Sniper, "Sniper" }, { battle::Role::Special, "Special" }, { battle::Role::Support, "Support" },
        { battle::Role::Tank, "Tank" },     { battle::Role::Warrior, "Warrior" },
    };
    m_last_oper_name.clear();

    auto role_iter = RoleNameType.find(role);

    std::vector<std::string> tasks;
    if (role_iter == RoleNameType.cend()) {
        tasks = { "BattleQuickFormationRole-All", "BattleQuickFormationRole-All-OCR" };
    }
    else {
        tasks = { "BattleQuickFormationRole-" + role_iter->second };
    }
    return ProcessTask(*this, tasks).set_retry_times(0).run();
}

bool asst::BattleFormationTask::click_support_role_table()
{
    static const std::unordered_map<battle::Role, std::string> RoleNameType = {
        { battle::Role::Caster, "Caster" }, { battle::Role::Medic, "Medic" },     { battle::Role::Pioneer, "Pioneer" },
        { battle::Role::Sniper, "Sniper" }, { battle::Role::Special, "Special" }, { battle::Role::Support, "Support" },
        { battle::Role::Tank, "Tank" },     { battle::Role::Warrior, "Warrior" },
    };
    auto role = m_support_unit.first;

    m_last_oper_name.clear();
    auto role_iter = RoleNameType.find(role);

    std::vector<std::string> tasks;
    if (role_iter == RoleNameType.cend()) {
        return false;
    }
    else if (role == battle::Role::Pioneer) {
        // support page should start with Pioneer selected
        return true;
    }
    else {
        tasks = { "BattleSupportFormationRole-" + role_iter->second };
    }
    return ProcessTask(*this, tasks).set_retry_times(0).run();
}

bool asst::BattleFormationTask::parse_formation()
{
    json::value info = basic_info_with_what("BattleFormation");
    auto& details = info["details"];
    auto& formation = details["formation"];

    auto* groups = &Copilot.get_data().groups;
    if (m_data_resource == DataResource::SSSCopilot) {
        groups = &SSSCopilot.get_data().groups;
    }

    for (const auto& [name, opers_vec] : *groups) {
        if (opers_vec.empty()) {
            continue;
        }
        formation.array_emplace(name);

        // 判断干员/干员组的职业，放进对应的分组
        bool same_role = true;
        battle::Role role = BattleData.get_role(opers_vec.front().name);
        for (const auto& oper : opers_vec) {
            same_role &= BattleData.get_role(oper.name) == role;
        }

        // for unknown, will use { "BattleQuickFormationRole-All", "BattleQuickFormationRole-All-OCR" }
        m_formation[same_role ? role : battle::Role::Unknown].emplace_back(opers_vec);
    }

    callback(AsstMsg::SubTaskExtraInfo, info);
    return true;
}

bool asst::BattleFormationTask::select_formation(int select_index)
{
    // 编队不会触发改名的区域有两组
    // 一组是上面的黑长条 260*9
    // 第二组是名字最左边和最右边的一块区域
    // 右边比左边窄，暂定为左边 10*58

    static const std::vector<std::string> select_formation_task = { "BattleSelectFormation1", "BattleSelectFormation2",
                                                                    "BattleSelectFormation3",
                                                                    "BattleSelectFormation4" };

    return ProcessTask { *this, { select_formation_task[select_index - 1] } }.run();
}

bool asst::BattleFormationTask::on_run_fails()
{
    return !_permanent_failed;
}
