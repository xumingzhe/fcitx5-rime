/*
 * SPDX-FileCopyrightText: 2017~2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "rimecandidate.h"
#include "rimeengine.h"
#include <cstring>
#include <fcitx-utils/log.h>
#include <fcitx/candidatelist.h>
#include <memory>
#include <rime_api.h>
#include <stdexcept>

namespace fcitx::rime {

RimeCandidateWord::RimeCandidateWord(RimeEngine *engine,
                                     const RimeCandidate &candidate, int idx)
    : engine_(engine), idx_(idx) {
    setText(Text{candidate.text});
    if (candidate.comment && candidate.comment[0]) {
        setComment(Text{candidate.comment});
    }
}

void RimeCandidateWord::select(InputContext *inputContext) const {
    if (auto *state = engine_->state(inputContext)) {
        state->selectCandidate(inputContext, idx_, /*global=*/false);
    }
}

void RimeCandidateWord::forget(RimeState *state) const {
#ifndef FCITX_RIME_NO_DELETE_CANDIDATE
    state->deleteCandidate(idx_, /*global=*/false);
#endif
}

namespace {

class RimeMultiPageCandidateWord : public CandidateWord {
public:
    RimeMultiPageCandidateWord(RimeEngine *engine,
                               const RimeCandidate &candidate,
                               int globalIdx)
        : engine_(engine), globalIdx_(globalIdx) {
        setText(Text{candidate.text});
        if (candidate.comment && candidate.comment[0]) {
            setComment(Text{candidate.comment});
        }
    }

    void select(InputContext *inputContext) const override {
        if (auto *state = engine_->state(inputContext)) {
            state->selectCandidate(inputContext, globalIdx_,
                                   /*global=*/true);
        }
    }

    int globalIdx() const { return globalIdx_; }

private:
    RimeEngine *engine_;
    int globalIdx_;
};

// Collect candidates for a given page using the Rime API.
// Returns the number of candidates collected.
int collectPageCandidates(rime_api_t *api, RimeSessionId session, int pageNum,
                          int pageSize,
                          const RimeContext *currentContext,
                          int numSelectKeys, const char *selectKeys,
                          std::vector<Text> &labels,
                          std::vector<std::unique_ptr<CandidateWord>> &words,
                          RimeEngine *engine) {
    if (currentContext && pageNum == currentContext->menu.page_no) {
        // Use the current context for the current page.
        const auto &menu = currentContext->menu;
        bool hasLabel =
            RIME_STRUCT_HAS_MEMBER(*currentContext,
                                   currentContext->select_labels) &&
            currentContext->select_labels;
        int count = menu.num_candidates;
        for (int i = 0; i < count; i++) {
            std::string label;
            if (i < menu.page_size && hasLabel) {
                label = currentContext->select_labels[i];
            } else if (i < numSelectKeys) {
                label = std::string(1, selectKeys[i]);
            } else {
                label = std::to_string((i + 1) % 10);
            }
            label.append(" ");
            labels.emplace_back(label);
            words.emplace_back(std::make_unique<RimeMultiPageCandidateWord>(
                engine, menu.candidates[i], pageNum * pageSize + i));
        }
        return count;
    }

    // For non-current pages, use the candidate list iterator.
    int startIdx = pageNum * pageSize;
    RimeCandidateListIterator iter;
    if (!api->candidate_list_from_index(session, &iter, startIdx)) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < pageSize; i++) {
        if (!api->candidate_list_next(&iter)) {
            break;
        }
        std::string label;
        if (i < numSelectKeys) {
            label = std::string(1, selectKeys[i]);
        } else {
            label = std::to_string((i + 1) % 10);
        }
        label.append(" ");
        labels.emplace_back(label);
        words.emplace_back(std::make_unique<RimeMultiPageCandidateWord>(
            engine, iter.candidate, startIdx + i));
        count++;
    }
    api->candidate_list_end(&iter);
    return count;
}

} // namespace

RimeMultiPageCandidateList::RimeMultiPageCandidateList(
    RimeEngine *engine, InputContext *ic, const RimeContext &currentContext,
    int windowStart, int windowSize)
    : engine_(engine), ic_(ic),
      pageSize_(currentContext.menu.page_size),
      currentPage_(currentContext.menu.page_no) {
    setPageable(this);
    setActionable(this);
    setMultiPage(this);

    const auto &menu = currentContext.menu;
    int numSelectKeys =
        menu.select_keys ? strlen(menu.select_keys) : 0;
    const char *selectKeys = menu.select_keys;

    auto *api = engine_->api();
    auto session = engine_->state(ic_)->session(false);

    // Determine the actual window range.
    int endPage = windowStart + windowSize;

    // Collect candidates for each page in the window.
    for (int p = windowStart; p < endPage; p++) {
        // Record page start before adding candidates for this page.
        pageStarts_.push_back(candidateWords_.size());

        int collected = collectPageCandidates(
            api, session, p, pageSize_,
            (p == currentPage_) ? &currentContext : nullptr, numSelectKeys,
            selectKeys, labels_, candidateWords_, engine_);

        if (collected == 0) {
            // No more candidates. Remove the page start we just added
            // and stop.
            pageStarts_.pop_back();
            endPage = p;
            break;
        }

        // Track the active page.
        if (p == currentPage_) {
            activePage_ = pageStarts_.size() - 1;
            // Set cursor to the highlighted candidate on the current page.
            int localCursor = menu.highlighted_candidate_index;
            if (localCursor >= 0) {
                cursor_ = pageStarts_.back() + localCursor;
            }
        }
    }

    // Determine hasPrev / hasNext based on whether there are more pages
    // outside the window.
    hasPrev_ = windowStart > 0;
    // hasNext: there's at least one candidate on the page after the window.
    if (endPage > windowStart) {
        int nextPageStart = endPage * pageSize_;
        RimeCandidateListIterator iter;
        hasNext_ = api->candidate_list_from_index(session, &iter,
                                                   nextPageStart) &&
                   api->candidate_list_next(&iter);
        if (hasNext_) {
            api->candidate_list_end(&iter);
        }
    } else {
        hasNext_ = false;
    }
}

void RimeMultiPageCandidateList::prev() {
    KeyEvent event(ic_, Key(FcitxKey_Page_Up));
    if (auto state = engine_->state(ic_)) {
        state->keyEvent(event);
    }
}

void RimeMultiPageCandidateList::next() {
    KeyEvent event(ic_, Key(FcitxKey_Page_Down));
    if (auto state = engine_->state(ic_)) {
        state->keyEvent(event);
    }
}

bool RimeMultiPageCandidateList::hasAction(
    const CandidateWord & /*candidate*/) const {
#ifndef FCITX_RIME_NO_DELETE_CANDIDATE
    return true;
#else
    return false;
#endif
}

std::vector<CandidateAction>
RimeMultiPageCandidateList::candidateActions(
    const CandidateWord & /*candidate*/) const {
    std::vector<CandidateAction> actions;
#ifndef FCITX_RIME_NO_DELETE_CANDIDATE
    CandidateAction action;
    action.setId(0);
    action.setText(_("Forget word"));
    actions.push_back(std::move(action));
#endif
    return actions;
}

void RimeMultiPageCandidateList::triggerAction(
    const CandidateWord &candidate, int id) {
    if (id != 0) {
        return;
    }
    if (auto state = engine_->state(ic_)) {
#ifndef FCITX_RIME_NO_DELETE_CANDIDATE
        if (const auto *multiCandidate =
                dynamic_cast<const RimeMultiPageCandidateWord *>(&candidate)) {
            state->deleteCandidate(multiCandidate->globalIdx(),
                                   /*global=*/true);
        }
#endif
    }
}
RimeGlobalCandidateWord::RimeGlobalCandidateWord(RimeEngine *engine,
                                                 const RimeCandidate &candidate,
                                                 int idx)
    : engine_(engine), idx_(idx) {
    setText(Text{candidate.text});
    if (candidate.comment && candidate.comment[0]) {
        setComment(Text{candidate.comment});
    }
}

void RimeGlobalCandidateWord::select(InputContext *inputContext) const {
    if (auto *state = engine_->state(inputContext)) {
        state->selectCandidate(inputContext, idx_, /*global=*/true);
    }
}

void RimeGlobalCandidateWord::forget(RimeState *state) const {
#ifndef FCITX_RIME_NO_DELETE_CANDIDATE
    state->deleteCandidate(idx_, /*global=*/true);
#endif
}

RimeCandidateList::RimeCandidateList(RimeEngine *engine, InputContext *ic,
                                     const RimeContext &context)
    : engine_(engine), ic_(ic), hasPrev_(context.menu.page_no != 0),
      hasNext_(!context.menu.is_last_page) {
    setPageable(this);
    setBulk(this);
    setActionable(this);
#ifndef FCITX_RIME_NO_HIGHLIGHT_CANDIDATE
    setBulkCursor(this);
#endif

    const auto &menu = context.menu;

    int num_select_keys = menu.select_keys ? strlen(menu.select_keys) : 0;
    bool has_label = RIME_STRUCT_HAS_MEMBER(context, context.select_labels) &&
                     context.select_labels;

    int i;
    for (i = 0; i < menu.num_candidates; ++i) {
        std::string label;
        if (i < menu.page_size && has_label) {
            label = context.select_labels[i];
        } else if (i < num_select_keys) {
            label = std::string(1, menu.select_keys[i]);
        } else {
            label = std::to_string((i + 1) % 10);
        }
        label.append(" ");
        labels_.emplace_back(label);

        candidateWords_.emplace_back(
            std::make_unique<RimeCandidateWord>(engine, menu.candidates[i], i));

        if (i == menu.highlighted_candidate_index) {
            cursor_ = i;
        }
    }
}

const CandidateWord &RimeCandidateList::candidateFromAll(int idx) const {
    if (idx < 0 || empty()) {
        throw std::invalid_argument("Invalid global index");
    }

    auto session = engine_->state(ic_)->session(false);
    if (!session) {
        throw std::invalid_argument("Invalid session");
    }

    auto index = static_cast<size_t>(idx);

    auto *api = engine_->api();

    RimeCandidateListIterator iter;
    if (index >= globalCandidateWords_.size()) {
        if (index >= maxSize_) {
            throw std::invalid_argument("Invalid global index");
        }
    } else {
        if (globalCandidateWords_[index]) {
            return *globalCandidateWords_[index];
        }
    }

    if (!api->candidate_list_from_index(session, &iter, idx) ||
        !api->candidate_list_next(&iter)) {
        maxSize_ = std::min(index, maxSize_);
        throw std::invalid_argument("Invalid global index");
    }

    if (index >= globalCandidateWords_.size()) {
        globalCandidateWords_.resize(index + 1);
    }
    globalCandidateWords_[index] =
        std::make_unique<RimeGlobalCandidateWord>(engine_, iter.candidate, idx);
    api->candidate_list_end(&iter);
    return *globalCandidateWords_[index];
}

int RimeCandidateList::totalSize() const { return -1; }

bool RimeCandidateList::hasAction(const CandidateWord & /*candidate*/) const {
#ifndef FCITX_RIME_NO_DELETE_CANDIDATE
    // We can always reset rime candidate's frequency.
    return true;
#else
    return false;
#endif
}

std::vector<CandidateAction>
RimeCandidateList::candidateActions(const CandidateWord & /*candidate*/) const {
    std::vector<CandidateAction> actions;
#ifndef FCITX_RIME_NO_DELETE_CANDIDATE
    CandidateAction action;
    action.setId(0);
    action.setText(_("Forget word"));
    actions.push_back(std::move(action));
#endif
    return actions;
}

void RimeCandidateList::triggerAction(const CandidateWord &candidate, int id) {
    if (id != 0) {
        return;
    }
    if (auto state = engine_->state(ic_)) {
        if (const auto *rimeCandidate =
                dynamic_cast<const RimeGlobalCandidateWord *>(&candidate)) {
            rimeCandidate->forget(state);
        } else if (const auto *rimeCandidate =
                       dynamic_cast<const RimeCandidateWord *>(&candidate)) {
            rimeCandidate->forget(state);
        }
    }
}

#ifndef FCITX_RIME_NO_HIGHLIGHT_CANDIDATE
int RimeCandidateList::globalCursorIndex() const {
    return -1; // No API available.
}

void RimeCandidateList::setGlobalCursorIndex(int index) {
    auto session = engine_->state(ic_)->session(false);
    if (!session) {
        return;
    }
    auto *api = engine_->api();
    api->highlight_candidate(session, index);
}
#endif
} // namespace fcitx::rime

