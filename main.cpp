#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// split file
//     - find line boundaries for N almost equal parts
// map - run M threads
//     - file section -> container
//     - sort container
// shuffle
//     - make R containers
//     - move strings from M conainers to R containers
// reduce
//     - run R threads

using split_positions_t = std::vector<std::streampos>;

split_positions_t split_pos(const char* file, size_t M) {
  const auto flen = fs::file_size(fs::absolute(file));
  const auto chunk_size = flen / M;
  std::ifstream f{file, std::ifstream::binary};
  std::vector<std::streampos> res;
  res.reserve(M + 1);
  res.emplace_back(0);
  for (size_t chunk = 1; chunk < M; ++chunk) {
    auto pos = chunk * chunk_size;
    std::string buff;
    f.seekg(pos, std::ios::beg);
    std::getline(f, buff);
    res.emplace_back(f.tellg());
  }
  res.emplace_back(flen);
  return res;
}

using item_t = std::pair<std::string, size_t>;
using map_t = std::vector<item_t>;
using map_ptr_t = std::unique_ptr<map_t>;
using cluster_t = std::vector<map_ptr_t>;

map_ptr_t mapper(const char* file,
                 std::streampos const begin,
                 std::streampos const end) {
  std::ifstream f{file, std::ifstream::binary};
  f.seekg(begin, std::ios::beg);
  auto res = std::make_unique<map_t>();
  for (auto pos = begin; pos < end; pos = f.tellg()) {
    std::string s;
    std::getline(f, s);
    std::transform(s.begin(), s.end(), s.begin(), [](auto c) -> char {
      if (!std::isgraph(c))
        return ' ';
      else
        return std::tolower(c);
    });
    s.erase(std::remove_if(s.begin(), s.end(), [](auto c) { return c == ' '; }),
            s.end());
    if (s.size() > 0) {
      res->emplace_back(item_t{std::move(s), 1});
    }
  }
  std::sort(res->begin(), res->end());
  return res;
}

cluster_t do_map(const char* file, const split_positions_t& pos) {
  const auto M = pos.size() - 1;
  std::vector<std::future<map_ptr_t>> map_tasks;
  for (size_t t = 0; t < M; ++t) {
    map_tasks.emplace_back(
        std::async(std::launch::async, mapper, file, pos[t], pos[t + 1]));
  }
  cluster_t res;
  for (auto& t : map_tasks) {
    res.emplace_back(std::move(t.get()));
  }
  return res;
}

cluster_t do_shuffle(cluster_t& m_cluster, size_t R) {
  // setup iterators to m-workers
  auto M = m_cluster.size();
  std::vector<std::pair<map_t::iterator, map_t::iterator>> m_it(M);
  for (size_t m = 0; m < M; ++m) {
    m_it[m].first = m_cluster[m]->begin();
    m_it[m].second = m_cluster[m]->end();
  }

  // setup r-cluster
  cluster_t res(R);
  for (auto& r : res) {
    r = std::make_unique<map_t>();
  };
  const auto total_lines =
      std::accumulate(m_cluster.begin(), m_cluster.end(), 0,
                      [](auto& s, auto& ptr) { return s + ptr->size(); });
  const auto r_size = total_lines / R;

  std::string empty_string{""}, *last_moved = &empty_string;
  for (auto i_line = 0; i_line < total_lines; ++i_line) {
    auto r_sn = std::min(i_line / r_size, R - 1);  // r-worker serial number
    auto it_it_min =
        std::min_element(m_it.begin(), m_it.end(),
                         [](auto& a, auto& b) { return *a.first < *b.first; });
    // store minimal string eliminating duplicates
    if (*last_moved != (it_it_min->first)->first) {  // not a duplicate?
      if (!res[r_sn]->size()) {
        // 1) add last line from previous container for comparision
        res[r_sn]->emplace_back(item_t{*last_moved, 1});
        if (r_sn) {
          // 2) add first line from this container to previous for comparision
          res[r_sn - 1]->emplace_back(*(it_it_min->first));
        }
      }
      res[r_sn]->emplace_back(std::move(*(it_it_min->first)));
      last_moved = &res[r_sn]->back().first;
    }
    auto m =
        std::distance(m_it.begin(), it_it_min);  // # m-worker donating line

    if (++(m_it[m].first) ==
        m_it[m].second) {  // end of m-worker, stop iterating it
      m_it.erase(it_it_min);
    }
  }
  // add empty string to the end of last comtainer 'caus it will be dropped by
  // reducer
  res[R - 1]->emplace_back(item_t{empty_string, 1});

  return res;
}

void reducer(map_t& r, int id = 0) {
// note: first and last lines will be dropped - they are either "" or duplicates
// added just to compare with previous/subsequent lines
  if (r.size() < 3) // nothing to do
    return;

  auto prefix_size = [](std::string& s1, std::string& s2) -> size_t {
    auto max_len = std::min(s1.size(), s2.size());
    for (size_t len = 0; len < max_len; ++len) {
      if (s1[len] != s2[len])
        return len;
    }
    return max_len;
  };

  std::string fn{"file_" + std::to_string(id) + ".txt"};
  std::ofstream f{fn};

  auto pref_to_next = prefix_size(r[0].first, r[1].first);
  r[0].second = std::min(pref_to_next + 1, r[0].first.size());
  // drop first line as it is a duplicate or empty line

  for (size_t i = 1; i < r.size() - 1; ++i) {  // drop last line
    auto pref_to_prev = pref_to_next;
    pref_to_next = prefix_size(r[i].first, r[i + 1].first);
    r[i].second =
        std::min(r[i].first.size(), std::max(pref_to_prev, pref_to_next) + 1);
    f << r[i].first << " " << r[i].second << '\n';
  }
}

void do_reduce(cluster_t& r_cluster) {
  // const auto R = r_cluster.size();
  std::vector<std::future<void>> reduce_tasks;
  int id{0};
  for (auto& r : r_cluster) {
    reduce_tasks.emplace_back(
        std::async(std::launch::async, reducer, std::ref(*r.get()), id++));
  }
  for (auto& t : reduce_tasks) {
    t.wait();
  }
  return;
}

void map_reduce(const char* file, size_t M, size_t R) {
  auto borders = split_pos(file, M);
  auto m_cluster = do_map(file, borders);
  auto r_cluster = do_shuffle(m_cluster, R);
  do_reduce(r_cluster);

  /*
    auto count{0};
    for (auto& wrk : r_cluster) {
      for (auto& [s, nc] : *wrk) {
        std::cout << (count++) << ":" << s << ":" << nc << std::endl;
      }
    }
    */
}

int main(int argc, char* argv[]) {
  std::cout << fs::current_path() << std::endl;

  if (argc < 4) {
    std::cerr << "Usage: mapreduce <source_file> <mnum> <rnum>\n";
    return EXIT_FAILURE;
  }
  try {
    map_reduce(argv[1], std::atoi(argv[2]), std::atoi(argv[3]));
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
