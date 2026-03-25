#include <atomic>
#include <iostream>
#include <utility>
#include <random>
#include <set>
#include <cassert>
#include <functional>
#include <thread>
#include <vector>
#include <chrono>
#include <string>
#include <stack>
#include <queue>

#ifndef COMMON_HPP
#define COMMON_HPP
#include "./common.h"
#endif

namespace SUB_ERIC_TREE
{
    struct Node;
    typedef std::atomic<Node *> Edge;
    const int MAX_KEY = 2147483647;

    struct PropagateStatus
    {
        std::atomic<PropagateStatus *> subscribedTo; // TODO atomic

        PropagateStatus() : subscribedTo(nullptr) {}

        void setDoneBit()
        {
            PropagateStatus *tmp = (PropagateStatus *)((uintptr_t)subscribedTo.load() | 1);
            subscribedTo.store(tmp);
        }
        void resetDoneBit()
        {
            PropagateStatus *tmp = (PropagateStatus *)((uintptr_t)subscribedTo.load() & ~1);
            subscribedTo.store(tmp);
        }
        bool readDoneBit()
        {
            return (uintptr_t)subscribedTo.load() & 1;
        }
        PropagateStatus *getCleanPtr()
        {
            return (PropagateStatus *)((uintptr_t)subscribedTo.load() & ~1);
        }
    };
    struct alignas(32) VersionInfo
    {
        // is_leaf for version node
        int key = 0;
        int min_key = -MAX_KEY;
        int max_key = MAX_KEY;
        long long sum = 0;
        VersionInfo *child[2] = {nullptr, nullptr};
    };

    struct alignas(16) BundledVersionPointer
    {
        VersionInfo *version = nullptr;
        PropagateStatus *status = nullptr;

        void operator=(const BundledVersionPointer &bvp) volatile
        {
            version = bvp.version;
            status = bvp.status;
        }

        BundledVersionPointer() {}
        BundledVersionPointer(VersionInfo *v) : version(v) {}
        BundledVersionPointer(VersionInfo *v, PropagateStatus *s) : version(v), status(s) {}
        BundledVersionPointer(const volatile BundledVersionPointer &bvp)
            : version(bvp.version), status(bvp.status)
        {
        }
        BundledVersionPointer(const BundledVersionPointer &bvp)
            : version(bvp.version),
              status(bvp.status)
        {
        }

        bool operator==(const BundledVersionPointer &bvp) const
        {
            return version == bvp.version && status == bvp.status;
        }
    };

    struct Node
    {
        // std::atomic<VersionInfo *> version;
        // std::atomic<PropagateStatus *> status;

        // std::atomic<BundledVersionPointer> version_status;
        atomic_128<BundledVersionPointer> version_status;

        alignas(128) std::atomic<bool> removed;
        my_mutex mtx;

        int key;
        bool is_leaf;

        Node(int k = -1, bool is_leaf = false) : key(k), is_leaf(is_leaf), removed(false) {}
    };

    struct LeafNode : Node
    {
        LeafNode(int k, int v) : Node(k, true)
        {
            auto *info = new VersionInfo();
            info->key = k;
            info->sum = v;
            info->min_key = info->max_key = k;
            // this->version.store(info);
            this->version_status.store(BundledVersionPointer(info));
        }
    };

    struct InternalNode : Node
    {
        Edge child[2]; // [~, key), [key, ~)
        InternalNode(int k, Node *l = nullptr, Node *r = nullptr) : Node(k, false)
        {
            child[0].store(l);
            child[1].store(r);

            auto *info = new VersionInfo();
            info->key = k;
            info->sum = 0;
            info->min_key = -MAX_KEY;
            info->max_key = MAX_KEY;
            if (l != nullptr)
            {
                VersionInfo *l_version = l->version_status.load().version;
                info->child[0] = l_version;
                info->sum += l_version->sum;
                info->min_key = info->max_key = l->key;
            }
            if (r != nullptr)
            {
                VersionInfo *r_version = r->version_status.load().version;
                info->child[1] = r_version;
                info->sum += r_version->sum;
                info->min_key = std::min(info->min_key, r->key);
                info->max_key = std::max(info->max_key, r->key);
            }
            this->version_status.store(BundledVersionPointer(info));
        }
    };

    struct SubEricTree : RangeQueryMap
    {
        InternalNode *root;
        SubEricTree() : root(new InternalNode(MAX_KEY))
        {
            LeafNode *left_sentinel = new LeafNode(-MAX_KEY, SENTINAL_VALUE);
            LeafNode *right_sentinel = new LeafNode(MAX_KEY, SENTINAL_VALUE);
            root->child[0].store(left_sentinel);
            root->child[1].store(right_sentinel);

            // Print info (is lock_free, is_always_lock_free, bytes)

            // std::cerr << "version_status.is_lock_free: " << root->version_status.is_lock_free() << std::endl;
            std::cerr << "child[0].is_lock_free: " << root->child[0].is_lock_free() << std::endl;

            // std::cerr << "version_status.is_always_lock_free: " << root->version_status.is_always_lock_free << std::endl;
            std::cerr << "child[0].is_always_lock_free: " << root->child[0].is_always_lock_free << std::endl;

            std::cerr << "(version_status *) bytes: " << sizeof((root->version_status).load()) << std::endl;
            std::cerr << "(child[0] *) bytes: " << sizeof((root->child[0]).load()) << std::endl;
        }

    private:
        auto find(int key, MyStack<InternalNode *> &path_stk)
        {
            InternalNode *gp = nullptr;
            int gp_dir = 1;
            InternalNode *p = root;
            int p_dir = 0;
            Node *l = p->child[p_dir].load();

            path_stk.clear();
            path_stk.push(p);

            while (!l->is_leaf)
            {
                gp = p;
                gp_dir = p_dir;
                p = (InternalNode *)l;
                path_stk.push(p);
                p_dir = p->key <= key ? 1 : 0;
                l = p->child[p_dir].load();
            }

            return std::make_tuple(gp, gp_dir, p, p_dir, (LeafNode *)l);
        }

        VersionInfo *make_updated_verion_info(InternalNode *nd, VersionInfo *left_version, VersionInfo *right_version)
        {
            VersionInfo *new_version = new VersionInfo();
            new_version->key = nd->key;
            new_version->child[0] = left_version;
            new_version->child[1] = right_version;
            new_version->sum = left_version->sum + right_version->sum;
            new_version->min_key = std::min(left_version->min_key, right_version->min_key);
            new_version->max_key = std::max(left_version->max_key, right_version->max_key);
            return new_version;
        }

        std::pair<Node *, VersionInfo *> load_child_version_atomic(const std::atomic<Node *> &child_box)
        {
            Node *child = child_box.load();
            VersionInfo *version = child->version_status.load().version;
            while (child != child_box.load())
            {
                child = child_box.load();
                version = child->version_status.load().version;
            }
            return {child, version};
        }

        PropagateStatus *refreshOnce(InternalNode *node, PropagateStatus *ps, std::vector<VersionInfo*> *toRetire)
        {
            BundledVersionPointer expected = node->version_status.load_atomic();
            std::pair<Node *, VersionInfo *> left = load_child_version_atomic(node->child[0]);
            std::pair<Node *, VersionInfo *> right = load_child_version_atomic(node->child[1]);

            VersionInfo *new_version = make_updated_verion_info(node, left.second, right.second);
            BundledVersionPointer new_bvp = BundledVersionPointer(new_version, ps);
            BundledVersionPointer old_val = node->version_status.compare_exchange(expected, new_bvp);
            if(expected == old_val) {
                toRetire->push_back(old_val.version);
                return ps;
            } else return old_val.status;
        }

        PropagateStatus *refreshTwiceNoDelegate(InternalNode *node, PropagateStatus *ps, std::vector<VersionInfo*> *toRetire)
        {
            PropagateStatus* ret = refreshOnce(node, ps, toRetire);
            if(ret == ps) return ps;
            refreshOnce(node, ps, toRetire);
            return ps;
        }

        PropagateStatus *refreshTwice(InternalNode *node, PropagateStatus *ps, std::vector<VersionInfo*> *toRetire)
        {
            PropagateStatus* ret = refreshOnce(node, ps, toRetire);
            if(ret == ps) return ps;
            return refreshOnce(node, ps, toRetire);
        }

        // PropagateStatus *refreshUntilConsistent(InternalNode *node, PropagateStatus *ps, std::vector<VersionInfo*> *toRetire) {
        //     while(true) {
        //         BundledVersionPointer expected = node->version_status.load_atomic();
        //         std::pair<Node *, VersionInfo *> left = load_child_version_atomic(node->child[0]);
        //         std::pair<Node *, VersionInfo *> right = load_child_version_atomic(node->child[1]);

        //         VersionInfo *new_version = make_updated_verion_info(node, left.second, right.second);
        //         BundledVersionPointer new_bvp = BundledVersionPointer(new_version, ps);
        //         BundledVersionPointer old_val = node->version_status.compare_exchange(expected, new_bvp);
        //         if(!(expected == old_val)) return old_val.status;
        //         toRetire->push_back(old_val.version);
        //         // std::cerr << "reached here\n";
        //         // return ps;
        //         if(left == load_child_version_atomic(node->child[0]) && right == load_child_version_atomic(node->child[1]))
        //             return ps;
        //     }
        // }

        // PropagateStatus *refreshUntilConsistent2(InternalNode *node, PropagateStatus *ps, std::vector<VersionInfo*> *toRetire) {
        //     BundledVersionPointer expected = node->version_status.load_atomic();
        //     std::pair<Node *, VersionInfo *> left = load_child_version_atomic(node->child[0]);
        //     std::pair<Node *, VersionInfo *> right = load_child_version_atomic(node->child[1]);
        //     while(true) {
        //         // expected = node->version_status.load_atomic();
        //         left = load_child_version_atomic(node->child[0]);
        //         right = load_child_version_atomic(node->child[1]);
        //         VersionInfo *new_version = make_updated_verion_info(node, left.second, right.second);
        //         BundledVersionPointer new_bvp = BundledVersionPointer(new_version, ps);
        //         BundledVersionPointer old_val = node->version_status.compare_exchange(expected, new_bvp);
        //         if(!(expected == old_val)) return old_val.status;
        //         toRetire->push_back(old_val.version);
        //         // std::cerr << "reached here\n";
        //         // return ps;
        //         if(left == load_child_version_atomic(node->child[0]) && right == load_child_version_atomic(node->child[1]))
        //             return ps;
        //         expected = new_bvp;
        //         left = load_child_version_atomic(node->child[0]);
        //         right = load_child_version_atomic(node->child[1]);
        //     }
        // }


        PropagateStatus *refreshUntilConsistent(InternalNode *node, PropagateStatus *ps, std::vector<VersionInfo*> *toRetire)
        {
            BundledVersionPointer existed_0, existed_1;
            existed_0 = node->version_status.load_atomic();
            std::pair<Node *, VersionInfo *> left;
            std::pair<Node *, VersionInfo *> right;
            std::pair<Node *, VersionInfo *> left2;
            std::pair<Node *, VersionInfo *> right2;

            left = load_child_version_atomic(node->child[0]);
            right = load_child_version_atomic(node->child[1]);

            do {
                left2 = left;
                right2 = right;
                existed_1 = existed_0;

                VersionInfo *new_version = make_updated_verion_info(node, left2.second, right2.second);
                BundledVersionPointer new_bvp = BundledVersionPointer(new_version, ps);
                existed_0 = node->version_status.compare_exchange(existed_1, new_bvp);
                if (!(existed_1 == existed_0)) {
                    return existed_0.status;
                } else {
                    toRetire->push_back(existed_0.version);
                }
                existed_0 = new_bvp;
                left = load_child_version_atomic(node->child[0]);
                right = load_child_version_atomic(node->child[1]);

            } while (left.second != left2.second || right.second != right2.second);

            return ps;
        }

        bool findRetiredNodes(VersionInfo* nd, std::vector<VersionInfo*>* toRetire, std::string msg = "") {
            int counter = 0;
            std::string s;
            for (int j = 0; j < toRetire->size(); j++) {
                if(findNode(nd, (*toRetire)[j])) {
                    counter++;
                    s += '1';
                }
                else{
                    s += '0';
                }
            }
            if(counter) {
                std::stringstream ss;
                ss << counter << " out of " << toRetire->size() << " versions are still reachable!! " << msg << std::endl;
                ss << s << std::endl;
                std::cerr << ss.str();
            } else {
                // std::stringstream ss;
                // ss << "None Reachable" << std::endl;
                // ss << s << std::endl;
                // std::cerr << ss.str();
            }
            return counter == 0;
        }

        void propagate(MyStack<InternalNode *> &path_stk)
        {
            PropagateStatus *ps = new PropagateStatus();
            std::vector<VersionInfo*> toRetire;
            toRetire.reserve(4000);
            while (!path_stk.empty())
            {
                InternalNode *nd = path_stk.pop();

                PropagateStatus *cur_ps = refreshUntilConsistent(nd, ps, &toRetire);
                if (cur_ps != ps) // failed; subscribe
                {
                    ps->subscribedTo = cur_ps;
                    break;
                }
                assert(findRetiredNodes(nd->version_status.load().version, &toRetire, "INTERNAL NODE"));
            }

            PropagateStatus *subscribedTo = ps->subscribedTo;
            if (subscribedTo != nullptr)
            {
                while (subscribedTo->readDoneBit() == false)
                    ;
            }
            ps->setDoneBit();
            VersionInfo* verRoot = root->version_status.load().version;
            assert(findRetiredNodes(verRoot, &toRetire));
        }

        bool findNode(VersionInfo* root, VersionInfo* node) {
            if(root == node) return true;
            if(root->child[0] == 0) return false;
            VersionInfo* child = node->key < root->key ? root->child[0] : root->child[1];
            return findNode(child, node);
        }

    public:
        bool insert(int key, int val)
        {
            while (true)
            {
                MyStack<InternalNode *> path_stk;
                auto [_, __, p, p_dir, leaf] = find(key, path_stk);
                if (leaf->key == key) // check leaf delete mark?
                {
                    propagate(path_stk);
                    return false;
                }

                bool success = p->mtx.try_lock();
                if (!success)
                    continue;
                Edge *ptr = &(p->child[p_dir]); // desired location
                if (p->removed.load() || ptr->load() != leaf)
                {
                    // p updated
                    p->mtx.unlock();
                    continue;
                }

                LeafNode *new_inserted_leaf = new LeafNode(key, val);
                LeafNode *new_existing_leaf = new LeafNode(leaf->key, leaf->version_status.load().version->sum);
                InternalNode *new_in_node =
                    (key < leaf->key)
                        ? new InternalNode(leaf->key, new_inserted_leaf, new_existing_leaf)
                        : new InternalNode(key, new_existing_leaf, new_inserted_leaf);

                // this should never fail because I locked p
                // bool result = ptr->compare_exchange_strong((Node *&)leaf, (Node *)new_in_node); // LinP for success
                ptr->store(new_in_node);

                p->mtx.unlock();
                propagate(path_stk); // This is fine?
                return true;
            }
        }

        int remove(int key)
        {
            LeafNode *prev_leaf = nullptr;
            while (true)
            {
                MyStack<InternalNode *> path_stk;
                auto [gp, gp_dir, p, p_dir, leaf] = find(key, path_stk);
                if (leaf->key != key)
                {
                    propagate(path_stk);
                    return SENTINAL_VALUE; // key not found
                }
                if (prev_leaf != nullptr && prev_leaf != leaf)
                {
                    propagate(path_stk);
                    return SENTINAL_VALUE; // key deleted and re-added
                }
                prev_leaf = leaf;

                bool success = gp->mtx.try_lock();
                if (!success)
                    continue;
                success = p->mtx.try_lock();
                if (!success)
                {
                    gp->mtx.unlock();
                    continue;
                }
                Edge *ptr = &(gp->child[gp_dir]);
                if (gp->removed.load() || ptr->load() != p)
                {
                    gp->mtx.unlock();
                    p->mtx.unlock();
                    continue;
                }
                Node *remaining_node = p->child[1 - p_dir].load();
                Node *target_leaf = p->child[p_dir].load();
                if (target_leaf != leaf)
                {
                    gp->mtx.unlock();
                    p->mtx.unlock();
                    continue;
                }

                p->removed.store(true);
                ptr->store(remaining_node); // LinP for success
                int removed_value = leaf->version_status.load().version->sum;

                // TODO remove nodes
                // delete p;
                // delete l;

                gp->mtx.unlock();
                p->mtx.unlock();

                path_stk.pop(); // we should not update p since it's deleted
                propagate(path_stk);
                return removed_value;
            }
        }

        int lookup(int key)
        {
            VersionInfo *vnd = root->version_status.load().version;

            while (vnd != nullptr && vnd->child[0] != nullptr)
            {
                if (key < vnd->min_key || vnd->max_key < key)
                    return SENTINAL_VALUE;
                if (key < vnd->key)
                    vnd = vnd->child[0];
                else
                    vnd = vnd->child[1];
            }
            bool success = vnd != nullptr && vnd->key == key;
            return success ? vnd->sum : SENTINAL_VALUE;
        }

        bool search(int key)
        {
            return lookup(key) != SENTINAL_VALUE;
        }

        long long range_sum(int key_st, int key_ed)
        {
            VersionInfo *vnd = root->version_status.load().version;

            while (vnd != nullptr)
            {
                int st_dir = vnd->key <= key_st ? 1 : 0;
                int ed_dir = vnd->key > key_ed ? 0 : 1;
                if (st_dir == ed_dir && vnd->child[st_dir] != nullptr)
                    vnd = vnd->child[st_dir];
                else
                    break;
            }

            // vnd is a split point (LCA of key_st and key_ed)

            if (vnd == nullptr)
            {
                return 0;
            }
            if (vnd->child[0] == nullptr && vnd->child[1] == nullptr)
            {
                if (key_st <= vnd->key && vnd->key <= key_ed)
                    return vnd->sum;
                return 0;
            }

            long long result = 0;
            int keys[2] = {key_st, key_ed};
            std::queue<std::pair<VersionInfo *, int>> q;
            q.push({vnd->child[0], 0});
            q.push({vnd->child[1], 1});

            while (!q.empty())
            {
                auto [cur_vnd, r_dir] = q.front();
                q.pop();
                if (cur_vnd == nullptr)
                    continue;

                if (key_ed < cur_vnd->min_key || cur_vnd->max_key < key_st)
                    continue;
                if (cur_vnd->min_key <= key_st && key_ed <= cur_vnd->max_key)
                {
                    result += cur_vnd->sum;
                    continue;
                }

                if (cur_vnd->child[0] == nullptr && cur_vnd->child[1] == nullptr)
                { // leaf
                    if (keys[0] <= cur_vnd->key && cur_vnd->key <= keys[1])
                        result += cur_vnd->sum;
                    continue;
                }

                int dir = -1;

                // [~~ , nd->key), [nd->key, ~~)
                if (r_dir == 0)
                {
                    dir = cur_vnd->key < keys[0] ? 1 : 0;
                }
                else
                {
                    dir = cur_vnd->key <= keys[1] ? 1 : 0;
                }

                // if I should go to r_dir, add the other subtree sum. if not, just go.
                // accessing to the wrong(old/new) edges?
                if (r_dir == dir)
                {
                    VersionInfo *other_subtree = cur_vnd->child[1 - dir];
                    result += other_subtree == nullptr ? 0 : other_subtree->sum;
                }
                q.push({cur_vnd->child[dir], r_dir});
            }

            return result;
        }

        /* Debugs */

        void iterate(std::function<void(LeafNode *)> f_leaf, std::function<void(InternalNode *)> f_internal)
        {
            std::queue<Node *> q;
            q.push(root);
            while (!q.empty())
            {
                Node *nd = q.front();
                q.pop();
                if (nd == nullptr)
                    continue;
                else if (nd->is_leaf)
                {
                    auto leaf = (LeafNode *)nd;
                    if (leaf->key != -MAX_KEY && leaf->key != MAX_KEY)
                        f_leaf(leaf);
                }
                else
                {
                    auto in = (InternalNode *)nd;
                    f_internal(in);
                    q.push(in->child[0].load());
                    q.push(in->child[1].load());
                }
            }
        }

        int size()
        {
            int cnt = 0;
            iterate([&cnt](LeafNode *leaf)
                    { cnt++; },
                    [](InternalNode *in) {});
            return cnt;
        }

        void print()
        {
            std::cout << "\n--- Printing tree ---" << std::endl;
            iterate([](LeafNode *leaf)
                    { std::cout << "Leaf\t " << leaf->key << " : " << leaf->version_status.load().version->sum << std::endl; },
                    [](InternalNode *in)
                    {
                        int my_current_sum = in->version_status.load().version->sum;
                        int left_key = in->child[0].load() == nullptr ? -1 : in->child[0].load()->key;
                        int right_key = in->child[1].load() == nullptr ? -1 : in->child[1].load()->key;
                        std::cout << "Internal\t " << in->key << " : " << my_current_sum << "\n  Left: " << left_key << ", Right: " << right_key << std::endl;
                    });
            std::cout << "--- End of tree ---\n"
                      << std::endl;
        }

        long long sum_keys()
        {
            long long sum = 0;
            iterate([&sum](LeafNode *leaf)
                    { sum += leaf->key; },
                    [](InternalNode *in) {});
            return sum;
        }

        long long sum_values()
        {
            long long sum = 0;
            iterate([&sum](LeafNode *leaf)
                    { sum += (leaf->version_status.load()).version->sum; },
                    [](InternalNode *in) {});
            return sum;
        }

        float average_depth()
        {
            float sum = 0;
            int size = 0;

            std::queue<std::pair<Node *, int>> q;
            q.push({root, 1});
            while (!q.empty())
            {
                auto [nd, dep] = q.front();
                q.pop();
                if (nd == nullptr)
                    continue;
                else if (nd->is_leaf)
                {
                    auto leaf = (LeafNode *)nd;
                    if (leaf->key != -MAX_KEY && leaf->key != MAX_KEY)
                    {
                        sum += dep;
                        size += 1;
                    }
                }
                else
                {
                    auto in = (InternalNode *)nd;
                    q.push({in->child[0].load(), dep + 1});
                    q.push({in->child[1].load(), dep + 1});
                }
            }
            return sum / size;
        }
    };
}