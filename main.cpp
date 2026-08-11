#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kMagic = 0x42505431; // BPT1
constexpr int kOrder = 50;
constexpr const char *kDatabaseFile = "bpt.db";

struct Key {
  char index[65]{};
  std::int32_t value{};
};

int compareIndex(const char *lhs, const char *rhs) {
  return std::strcmp(lhs, rhs);
}

bool operator<(const Key &lhs, const Key &rhs) {
  const int comparison = compareIndex(lhs.index, rhs.index);
  return comparison < 0 || (comparison == 0 && lhs.value < rhs.value);
}

bool operator==(const Key &lhs, const Key &rhs) {
  return lhs.value == rhs.value && compareIndex(lhs.index, rhs.index) == 0;
}

Key makeKey(const std::string &index, int value) {
  Key key{};
  std::memcpy(key.index, index.data(), index.size());
  key.value = value;
  return key;
}

struct Node {
  std::uint16_t count = 0;
  std::uint8_t leaf = 1;
  std::uint8_t unused = 0;
  std::uint32_t next = 0;
  std::array<Key, kOrder> keys{};
  std::array<std::uint32_t, kOrder + 1> children{};
};

struct Header {
  std::uint32_t magic = kMagic;
  std::uint32_t root = 0;
  std::uint32_t nodeCount = 1;
  std::uint32_t reserved = 0;
  std::uint64_t entryCount = 0;
};

struct Split {
  bool happened = false;
  Key separator{};
  std::uint32_t right = 0;
};

class BPlusTree {
public:
  BPlusTree() { load(); }

  ~BPlusTree() { save(); }

  void insert(const std::string &index, int value) {
    const Key key = makeKey(index, value);
    bool inserted = false;
    const Split split = insertInto(header_.root, key, inserted);
    if (!inserted) return;

    ++header_.entryCount;
    if (split.happened) {
      Node root;
      root.leaf = 0;
      root.count = 1;
      root.keys[0] = split.separator;
      root.children[0] = header_.root;
      root.children[1] = split.right;
      header_.root = addNode(root);
    }
  }

  void erase(const std::string &index, int value) {
    const Key key = makeKey(index, value);
    const std::uint32_t leafId = findLeaf(key);
    Node &leaf = nodes_[leafId];
    const int position = lowerBound(leaf, key);
    if (position == leaf.count || !(leaf.keys[position] == key)) return;

    for (int i = position + 1; i < leaf.count; ++i) leaf.keys[i - 1] = leaf.keys[i];
    --leaf.count;
    --header_.entryCount;

  }

  void find(const std::string &index) const {
    const Key first = makeKey(index, std::numeric_limits<int>::min());
    std::uint32_t leafId = findLeaf(first);
    int position = lowerBound(nodes_[leafId], first);
    bool found = false;

    while (true) {
      const Node &leaf = nodes_[leafId];
      for (; position < leaf.count; ++position) {
        const int comparison = compareIndex(leaf.keys[position].index, index.c_str());
        if (comparison > 0) {
          finishOutput(found);
          return;
        }
        if (comparison == 0) {
          if (found) std::cout << ' ';
          std::cout << leaf.keys[position].value;
          found = true;
        }
      }
      if (leaf.next == 0) break;
      leafId = leaf.next;
      position = 0;
    }
    finishOutput(found);
  }

private:
  Header header_;
  std::vector<Node> nodes_;

  static void finishOutput(bool found) {
    if (!found) std::cout << "null";
    std::cout << '\n';
  }

  void load() {
    std::ifstream input(kDatabaseFile, std::ios::binary);
    Header stored;
    if (!input.read(reinterpret_cast<char *>(&stored), sizeof(stored)) ||
        stored.magic != kMagic || stored.nodeCount == 0) {
      initialize();
      return;
    }

    std::vector<Node> loaded(stored.nodeCount);
    if (!input.read(reinterpret_cast<char *>(loaded.data()),
                    static_cast<std::streamsize>(loaded.size() * sizeof(Node)))) {
      initialize();
      return;
    }
    header_ = stored;
    nodes_ = std::move(loaded);
  }

  void initialize() {
    header_ = Header{};
    nodes_.assign(1, Node{});
  }

  void save() {
    header_.nodeCount = static_cast<std::uint32_t>(nodes_.size());
    const std::string temporary = std::string(kDatabaseFile) + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(&header_), sizeof(header_));
    output.write(reinterpret_cast<const char *>(nodes_.data()),
                 static_cast<std::streamsize>(nodes_.size() * sizeof(Node)));
    output.close();
    std::remove(kDatabaseFile);
    std::rename(temporary.c_str(), kDatabaseFile);
  }

  std::uint32_t addNode(const Node &node) {
    nodes_.push_back(node);
    return static_cast<std::uint32_t>(nodes_.size() - 1);
  }

  static int lowerBound(const Node &node, const Key &key) {
    int left = 0, right = node.count;
    while (left < right) {
      const int middle = (left + right) / 2;
      if (node.keys[middle] < key)
        left = middle + 1;
      else
        right = middle;
    }
    return left;
  }

  std::uint32_t findLeaf(const Key &key) const {
    std::uint32_t id = header_.root;
    while (!nodes_[id].leaf) {
      const Node &node = nodes_[id];
      int child = 0;
      while (child < node.count && !(key < node.keys[child])) ++child;
      id = node.children[child];
    }
    return id;
  }

  Split insertInto(std::uint32_t nodeId, const Key &key, bool &inserted) {
    if (nodes_[nodeId].leaf) return insertIntoLeaf(nodeId, key, inserted);

    int childPosition = 0;
    while (childPosition < nodes_[nodeId].count &&
           !(key < nodes_[nodeId].keys[childPosition]))
      ++childPosition;
    const std::uint32_t childId = nodes_[nodeId].children[childPosition];
    const Split childSplit = insertInto(childId, key, inserted);
    if (!childSplit.happened) return {};

    std::array<Key, kOrder + 1> keys;
    std::array<std::uint32_t, kOrder + 2> children;
    const Node &old = nodes_[nodeId];
    for (int i = 0; i < childPosition; ++i) keys[i] = old.keys[i];
    keys[childPosition] = childSplit.separator;
    for (int i = childPosition; i < old.count; ++i) keys[i + 1] = old.keys[i];
    for (int i = 0; i <= childPosition; ++i) children[i] = old.children[i];
    children[childPosition + 1] = childSplit.right;
    for (int i = childPosition + 1; i <= old.count; ++i)
      children[i + 1] = old.children[i];

    const int total = old.count + 1;
    if (total <= kOrder) {
      Node &node = nodes_[nodeId];
      node.count = total;
      for (int i = 0; i < total; ++i) node.keys[i] = keys[i];
      for (int i = 0; i <= total; ++i) node.children[i] = children[i];
      return {};
    }

    const int middle = total / 2;
    Node right;
    right.leaf = 0;
    right.count = total - middle - 1;
    for (int i = 0; i < right.count; ++i) right.keys[i] = keys[middle + 1 + i];
    for (int i = 0; i <= right.count; ++i) right.children[i] = children[middle + 1 + i];

    Node &left = nodes_[nodeId];
    left.count = middle;
    for (int i = 0; i < middle; ++i) left.keys[i] = keys[i];
    for (int i = 0; i <= middle; ++i) left.children[i] = children[i];
    return {true, keys[middle], addNode(right)};
  }

  Split insertIntoLeaf(std::uint32_t nodeId, const Key &key, bool &inserted) {
    const int position = lowerBound(nodes_[nodeId], key);
    if (position < nodes_[nodeId].count && nodes_[nodeId].keys[position] == key) return {};
    inserted = true;

    std::array<Key, kOrder + 1> keys;
    const Node &old = nodes_[nodeId];
    for (int i = 0; i < position; ++i) keys[i] = old.keys[i];
    keys[position] = key;
    for (int i = position; i < old.count; ++i) keys[i + 1] = old.keys[i];
    const int total = old.count + 1;

    if (total <= kOrder) {
      Node &leaf = nodes_[nodeId];
      leaf.count = total;
      for (int i = 0; i < total; ++i) leaf.keys[i] = keys[i];
      return {};
    }

    const int leftCount = total / 2;
    Node right;
    right.leaf = 1;
    right.count = total - leftCount;
    right.next = old.next;
    for (int i = 0; i < right.count; ++i) right.keys[i] = keys[leftCount + i];
    const std::uint32_t rightId = addNode(right);

    Node &left = nodes_[nodeId];
    left.count = leftCount;
    left.next = rightId;
    for (int i = 0; i < leftCount; ++i) left.keys[i] = keys[i];
    return {true, right.keys[0], rightId};
  }

  void rebuild() {
    std::vector<Key> entries;
    entries.reserve(static_cast<std::size_t>(header_.entryCount));
    std::uint32_t leaf = header_.root;
    while (!nodes_[leaf].leaf) leaf = nodes_[leaf].children[0];
    while (true) {
      for (int i = 0; i < nodes_[leaf].count; ++i) entries.push_back(nodes_[leaf].keys[i]);
      if (nodes_[leaf].next == 0) break;
      leaf = nodes_[leaf].next;
    }

    const std::uint64_t count = header_.entryCount;
    initialize();
    for (const Key &key : entries) {
      bool inserted = false;
      const Split split = insertInto(header_.root, key, inserted);
      if (split.happened) {
        Node root;
        root.leaf = 0;
        root.count = 1;
        root.keys[0] = split.separator;
        root.children[0] = header_.root;
        root.children[1] = split.right;
        header_.root = addNode(root);
      }
    }
    header_.entryCount = count;
  }
};

} // namespace

int main() {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  BPlusTree tree;
  int commandCount;
  std::cin >> commandCount;
  while (commandCount--) {
    std::string command, index;
    std::cin >> command >> index;
    if (command == "find") {
      tree.find(index);
    } else {
      int value;
      std::cin >> value;
      if (command == "insert")
        tree.insert(index, value);
      else
        tree.erase(index, value);
    }
  }
  return 0;
}
