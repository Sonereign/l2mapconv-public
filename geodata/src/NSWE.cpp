#include "pch.h"

#include "NSWE.h"

//#define DISABLE_SIMPLE_NSWE_CALCULATION

namespace geodata {

static constexpr auto GRAVITY_STEP_DELTA = 1.0f;

void mark_walkable_triangles(float walkable_angle, const float *vertices,
                             const int *triangles, std::size_t triangle_count,
                             unsigned char *areas);

auto triangles_at_columns(const rcHeightfield &hf, const Map &map,
                          const std::vector<std::vector<int>> &triangle_index,
                          int x, int y, int radius = 0)
    -> std::vector<geometry::Triangle>;

inline auto allow_direction(int area, int direction) -> int {
  return area | 1 << (direction + 2);
}

inline auto forbid_direction(int area, int direction) -> int {
  return area & ~(1 << (direction + 2));
}

inline auto change_area(int area, int new_area) -> int {
  return new_area | unpack_nswe(area) << 2;
}

inline auto direction_forbidden(int area, int direction) -> bool {
  return (area >> 2 & (1 << direction)) == 0;
}

NSWE::NSWE(const Map &map, float cell_size, float cell_height,
           float walkable_height, float walkable_angle,
           float min_walkable_climb, float max_walkable_climb)
    : m_map{map}, m_cell_size{cell_size}, m_cell_height{cell_height},
      m_walkable_height{walkable_height}, m_walkable_angle_radians{std::cos(
                                              glm::radians(walkable_angle))},
      m_min_walkable_climb{min_walkable_climb},
      m_max_walkable_climb{max_walkable_climb}, m_hf{rcAllocHeightfield()} {

  build_filtered_heightfield();
}

NSWE::~NSWE() { rcFreeHeightField(m_hf); }

auto NSWE::calculate_nswe() -> const rcHeightfield & {

#ifndef DISABLE_SIMPLE_NSWE_CALCULATION
  calculate_simple_nswe();
#endif

  //  calculate_complex_nswe();

  return *m_hf;
}

void NSWE::build_filtered_heightfield() {

  const auto *bb_min = glm::value_ptr(m_map.internal_bounding_box().min());
  const auto *bb_max = glm::value_ptr(m_map.internal_bounding_box().max());

  // Grid size
  auto width = 0;
  auto height = 0;
  rcCalcGridSize(bb_min, bb_max, m_cell_size, &width, &height);

  // Create heightfield
  rcContext context{};
  rcCreateHeightfield(&context, *m_hf, width, height, bb_min, bb_max,
                      m_cell_size, m_cell_height);

  // Prepare geometry data
  const auto *vertices = glm::value_ptr(m_map.vertices().front());
  const auto vertex_count = m_map.vertices().size();
  const auto *triangles = reinterpret_cast<const int *>(m_map.indices().data());
  const auto triangle_count = m_map.indices().size() / 3;

  // Rasterize triangles
  m_triangle_index.reserve(width * height);
  std::vector<unsigned char> areas(triangle_count);
  mark_walkable_triangles(vertices, triangles, triangle_count, &areas.front());
  rcRasterizeTriangles(&context, vertices, vertex_count, triangles,
                       &areas.front(), triangle_count, *m_hf,
                       &m_triangle_index.front(), 1);

  // Filter spans
  rcFilterWalkableLowHeightSpans(
      &context, static_cast<int>(m_walkable_height / m_cell_height), *m_hf);
}

void NSWE::calculate_simple_nswe() {
  const auto walkable_height_cells =
      static_cast<int>(m_walkable_height / m_cell_height);
  const auto min_walkable_climb_cells =
      static_cast<int>(m_min_walkable_climb / m_cell_height);
  const auto max_walkable_climb_cells =
      static_cast<int>(m_max_walkable_climb / m_cell_height);

  const auto max_height = 0xffff;

  for (auto y = 0; y < m_hf->height; ++y) {
    for (auto x = 0; x < m_hf->width; ++x) {
      for (auto *span = m_hf->spans[x + y * m_hf->width]; span != nullptr;
           span = span->next) {

        const auto area = unpack_area(span->area);

        if (area == RC_NULL_AREA) {
          continue;
        }

        const auto bottom = static_cast<int>(span->smax);
        const auto top = span->next != nullptr
                             ? static_cast<int>(span->next->smin)
                             : max_height;

        for (auto direction = 0; direction < 4; ++direction) {
          const auto side_x = x + rcGetDirOffsetX(direction);
          const auto side_y = y + rcGetDirOffsetY(direction);

          // Allow moving outside of the map
          if (side_x < 0 || side_y < 0 || side_x >= m_hf->width ||
              side_y >= m_hf->height) {

            span->area = allow_direction(span->area, direction);
            continue;
          }

          auto direction_allowed = false;

          for (auto *neighbour = m_hf->spans[side_x + side_y * m_hf->width];
               neighbour != nullptr; neighbour = neighbour->next) {

            const auto neighbour_bottom = static_cast<int>(neighbour->smax);
            const auto neighbour_top =
                neighbour->next != nullptr
                    ? static_cast<int>(neighbour->next->smin)
                    : max_height;

            const auto height = std::min(top, neighbour_top) -
                                std::max(bottom, neighbour_bottom);
            const auto diff = neighbour_bottom - bottom;

            if (height > walkable_height_cells) {
              // Mark complex areas for further sphere-to-mesh collision
              // detection
              if (std::abs(diff) >= min_walkable_climb_cells / 2 &&
                  std::abs(diff) <= max_walkable_climb_cells) {

                span->area = change_area(span->area, RC_COMPLEX_AREA);
              }

              const auto neighbour_area = unpack_area(neighbour->area);

              if (area <= RC_STEEP_AREA || neighbour_area <= RC_STEEP_AREA) {
                // Forbid going up on steep surfaces
                direction_allowed = diff <= 0;
              } else {
                // Allow direction if height difference is small
                direction_allowed = diff <= max_walkable_climb_cells;
              }

              break;
            }
          }

          if (direction_allowed) {
            span->area = allow_direction(span->area, direction);
          }
        }
      }
    }
  }
}

void NSWE::calculate_complex_nswe() {
  for (auto y = 0; y < m_hf->height; ++y) {
    for (auto x = 0; x < m_hf->width; ++x) {
      for (auto *span = m_hf->spans[x + y * m_hf->width]; span != nullptr;
           span = span->next) {

#ifndef DISABLE_SIMPLE_NSWE_CALCULATION
        const auto area = unpack_area(span->area);

        if (area != RC_COMPLEX_AREA) {
          continue;
        }
#endif

        for (auto direction = 0; direction < 4; ++direction) {
#ifndef DISABLE_SIMPLE_NSWE_CALCULATION
          // Skip collision checking if direction is already forbidden at the
          // simple NSWE calculation step
          if (direction_forbidden(span->area, direction)) {
            continue;
          }
#endif

          const auto dx = rcGetDirOffsetX(direction);
          const auto dy = rcGetDirOffsetY(direction);
          const auto side_x = x + dx;
          const auto side_y = y + dy;

          if (side_x < 0 || side_y < 0 || side_x >= m_hf->width ||
              side_y >= m_hf->height) {

            continue;
          }

          if (slide_sphere_until_collision(x, y, span->smax, direction)) {
            span->area = forbid_direction(span->area, direction);
          } else {
#ifdef DISABLE_SIMPLE_NSWE_CALCULATION
            span->area = allow_direction(span->area, direction);
#endif
          }
        }
      }
    }
  }
}

void NSWE::mark_walkable_triangles(const float *vertices, const int *triangles,
                                   std::size_t triangle_count,
                                   unsigned char *areas) const {

  for (std::size_t i = 0; i < triangle_count; ++i) {
    const auto *triangle = &triangles[i * 3];
    const auto normal =
        glm::triangleNormal(glm::make_vec3(&vertices[triangle[0] * 3]),
                            glm::make_vec3(&vertices[triangle[1] * 3]),
                            glm::make_vec3(&vertices[triangle[2] * 3]));

    const auto slope = glm::dot(normal, {0.0f, 1.0f, 0.0f});

    if (slope < -0.01f) { // -0.01f workaround for "nearly vertical" surfaces
      areas[i] = RC_NULL_AREA;
    } else if (slope < m_walkable_angle_radians) {
      areas[i] = RC_STEEP_AREA;
    } else {
      areas[i] = RC_FLAT_AREA;
    }
  }
}

auto NSWE::slide_sphere_until_collision(int x, int y, int z,
                                        int direction) const -> bool {

  static constexpr auto delta = 1.0f;

  const auto map_origin = m_map.bounding_box().min();
  const auto sphere_radius = m_cell_size / 2.0f;

  // Place sphere on the cell
  const glm::vec3 sphere_center{
      map_origin.x + x * m_cell_size + m_cell_size / 2.0f,
      map_origin.z + z * m_cell_height +
          sphere_radius, // Z-up swapped with Y-up
      map_origin.y + y * m_cell_size + m_cell_size / 2.0f,
  };

  geometry::Sphere sphere{sphere_center, sphere_radius};

  const auto triangles = triangles_at_columns(x, y, 1);

  const auto dx = rcGetDirOffsetX(direction);
  const auto dy = rcGetDirOffsetY(direction);

  for (auto i = 0; i < static_cast<int>(m_cell_size / delta); ++i) {
    drop_sphere(sphere, triangles);

    sphere.center.x += dx * delta;
    sphere.center.z += dy * delta;

    if (sphere.intersects(triangles, std::less_equal(), 0.7f)) {
      return true;
    }

    sphere.center.y += delta;
  }

  return false;
}

void NSWE::drop_sphere(geometry::Sphere &sphere,
                       const std::vector<geometry::Triangle> &triangles) const {

  const auto original_z = sphere.center.y;

  while (original_z - sphere.center.y < m_max_walkable_climb * 2.0f) {
    if (sphere.intersects(triangles, std::less_equal(), 1.0f)) {
      if (sphere.center.y != original_z) {
        sphere.center.y += GRAVITY_STEP_DELTA;
      }

      return;
    }

    sphere.center.y -= GRAVITY_STEP_DELTA;
  }
}

auto NSWE::triangles_at_columns(int x, int y, int radius) const
    -> std::vector<geometry::Triangle> {

  std::vector<geometry::Triangle> triangles;
  std::unordered_set<int> column_indices;

  const auto &map_vertices = m_map.vertices();
  const auto &map_triangles = m_map.indices();

  // Find triangle indices
  for (auto dy = y - radius; dy < y + radius + 1; ++dy) {
    for (auto dx = x - radius; dx < x + radius + 1; ++dx) {
      if (dx < 0 || dy < 0 || dx >= m_hf->width || dy >= m_hf->height) {
        continue;
      }

      const auto &indices = m_triangle_index[dx + dy * m_hf->width];
      column_indices.insert(indices.cbegin(), indices.cend());
    }
  }

  // Make triangles by found indices
  for (const auto index : column_indices) {
    const geometry::Triangle triangle{
        map_vertices[map_triangles[index * 3 + 0]],
        map_vertices[map_triangles[index * 3 + 1]],
        map_vertices[map_triangles[index * 3 + 2]],
    };

    triangles.push_back(triangle);
  }

  return triangles;
}

} // namespace geodata
