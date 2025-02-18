#pragma once

#include "Entity.h"

#include <utils/NonCopyable.h>

#include <geometry/Box.h>
#include <geometry/Intersection.h>
#include <geometry/Sphere.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace geodata {

// Input coordinate system is converted from Z-up to Y-up
class Map : public utils::NonCopyable {
public:
  explicit Map(const std::string &name, const geometry::Box &bounding_box);
  Map(Map &&other) noexcept;

  void add(const Entity &entity);

  auto name() const -> const std::string &;
  auto bounding_box() const -> geometry::Box;

  auto internal_bounding_box() const -> const geometry::Box &;

  auto vertices() const -> const std::vector<glm::vec3> &;
  auto indices() const -> const std::vector<unsigned int> &;

private:
  const std::string m_name;
  const geometry::Box m_bounding_box;
  std::vector<glm::vec3> m_vertices;
  std::vector<unsigned int> m_indices;
};

} // namespace geodata
