/*
 * This file is part of the rRACES (https://github.com/caravagnalab/rRACES/).
 * Copyright (c) 2023 Alberto Casagrande <alberto.casagrande@uniud.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <vector>
#include <set>
#include <algorithm>
#include <filesystem>

#include <Rcpp.h>

#include "simulation.hpp"
#include "ending_conditions.hpp"

#include "phylogenetic_forest.hpp"

using namespace Rcpp;

template<typename SIMULATION_TEST>
struct RTest : public SIMULATION_TEST
{
  size_t counter;

  template<typename ...Args>
  explicit RTest(Args...args):
      SIMULATION_TEST(args...), counter{0}
  {}

  bool operator()(const Races::Drivers::Simulation::Simulation& simulation)
  {
    if (++counter >= 10000) {
      counter = 0;
      try {
        Rcpp::checkUserInterrupt();
      } catch (...) {
        return true;
      }
    }

    using namespace Races::Drivers::Simulation;

    return SIMULATION_TEST::operator()(simulation);
  }
};

const std::map<std::string, Races::Drivers::CellEventType> event_names{
  {"death",  Races::Drivers::CellEventType::DEATH},
  {"growth", Races::Drivers::CellEventType::DUPLICATION},
  {"switch", Races::Drivers::CellEventType::EPIGENETIC_SWITCH},
};

size_t count_events(const Races::Drivers::Simulation::SpeciesStatistics& statistics,
                    const Races::Drivers::CellEventType& event)
{
  switch(event)
  {
    case Races::Drivers::CellEventType::DEATH:
      return statistics.killed_cells;
    case Races::Drivers::CellEventType::DUPLICATION:
      return statistics.num_duplications;
    case Races::Drivers::CellEventType::EPIGENETIC_SWITCH:
      return statistics.num_of_epigenetic_events();
    default:
      ::Rf_error("get_counts: unsupported event");
  }
}

inline std::string get_signature_string(const Races::Drivers::Simulation::Species& species)
{
  const auto& signature = species.get_methylation_signature();
  return Races::Drivers::GenotypeProperties::signature_to_string(signature);
}

void handle_unknown_event(const std::string& event)
{
  std::ostringstream oss;

  oss << "Event \"" << event << "\" is not supported. " << std::endl
      << "Supported events are ";

  size_t i{0};
  for (const auto& [name, type] : event_names) {
    if (i>0) {
      if (event_names.size()!=2) {
        oss << ",";
      }
      oss << " ";
    }

    if ((++i)==event_names.size()) {
      oss << "and ";
    }

    oss << "\"" << name << "\"";
  }

  oss << ".";

  throw std::domain_error(oss.str());
}

std::set<Races::Drivers::SpeciesId>
get_species_ids_from_genotype_name(const Races::Drivers::Simulation::Tissue& tissue,
                                   const std::set<std::string>& genotype_name)
{
  std::set<Races::Drivers::SpeciesId > species_ids;

  for (const auto& species: tissue) {
    if (genotype_name.count(species.get_genotype_name())>0) {
      species_ids.insert(species.get_id());
    }
  }

  return species_ids;
}

Races::Drivers::Simulation::PositionInTissue
get_position_in_tissue(const std::vector<Races::Drivers::Simulation::AxisPosition>& position)
{
  if (position.size()==2) {
    return {position[0], position[1]};
  }

  ::Rf_error("rRACES supports only 2 dimensional space so far");
}

Races::Drivers::RectangleSet
get_rectangle(const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
              const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner)
{
  auto l_position = get_position_in_tissue(lower_corner);
  auto u_position = get_position_in_tissue(upper_corner);

  return {l_position, u_position};
}

size_t count_driver_mutated_cells(const Races::Drivers::Simulation::Tissue& tissue,
                                  const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                                  const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner,
                                  const std::set<Races::Drivers::SpeciesId>& species_filter,
                                  const std::set<std::string>& epigenetic_filter)
{
  using namespace Races::Drivers;
  using namespace Races::Drivers::Simulation;

  if (lower_corner.size() != upper_corner.size()) {
    ::Rf_error("lower_corner and upper_corner must have the same size");
  }

  auto lower_it = lower_corner.begin();
  auto upper_it = upper_corner.begin();
  for (;lower_it!=lower_corner.end();++lower_it,++upper_it)
  {
    if (*lower_it>*upper_it) {
      return 0;
    }
  }

  size_t total{0};
  for (auto x=lower_corner[0]; x<=upper_corner[0]; ++x) {
    for (auto y=lower_corner[1]; y<=upper_corner[1]; ++y) {
      auto cell_proxy = tissue({x,y});
      if (!cell_proxy.is_wild_type()) {
        const CellInTissue& cell = cell_proxy;

        if (species_filter.count(cell.get_species_id())>0) {

          const auto& species = tissue.get_species(cell.get_species_id());
          auto sign_string = get_signature_string(species);

          if (epigenetic_filter.count(sign_string)>0) {
            ++total;
          }
        }
      }
    }
  }

  return total;
}

std::vector<Races::Drivers::Simulation::Direction> get_possible_directions()
{
  namespace RS = Races::Drivers::Simulation;

  std::vector<RS::Direction> directions;
  for (const auto &x_move : {RS::Direction::X_UP, RS::Direction::X_DOWN, RS::Direction::X_NULL}) {
      for (const auto &y_move : {RS::Direction::Y_UP, RS::Direction::Y_DOWN, RS::Direction::Y_NULL}) {
          directions.push_back(x_move|y_move);
      }
  }

  // remove null move
  directions.pop_back();

  return directions;
}

struct PlainChooser
{
  std::shared_ptr<Races::Drivers::Simulation::Simulation> sim_ptr;
  std::string genotype_name;

  PlainChooser(const std::shared_ptr<Races::Drivers::Simulation::Simulation>& sim_ptr,
               const std::string& genotype_name):
    sim_ptr(sim_ptr), genotype_name(genotype_name)
  {}

  inline const Races::Drivers::Simulation::CellInTissue& operator()()
  {
    return sim_ptr->choose_cell_in(genotype_name,
                                   Races::Drivers::CellEventType::DUPLICATION);
  }
};

struct RectangularChooser : public PlainChooser
{
  Races::Drivers::RectangleSet rectangle;

  RectangularChooser(const std::shared_ptr<Races::Drivers::Simulation::Simulation>& sim_ptr,
                     const std::string& genotype_name,
                     const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                     const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner):
    PlainChooser(sim_ptr, genotype_name), rectangle(get_rectangle(lower_corner, upper_corner))
  {}

  inline const Races::Drivers::Simulation::CellInTissue& operator()()
  {
    return sim_ptr->choose_cell_in(genotype_name, rectangle,
                                   Races::Drivers::CellEventType::DUPLICATION);
  }
};

class SamplesForest;

//' @name TissueRectangle
//' @title A rectangle in the tissue
//' @field get_lower_corner Get the rectangle lower corner
//' @field get_upper_corner Get the rectangle upper corner
class TissueRectangle : public Races::Drivers::RectangleSet
{
public:
  TissueRectangle(const Races::Drivers::Simulation::PositionInTissue& lower_corner, 
                  const Races::Drivers::Simulation::PositionInTissue& upper_corner);

  TissueRectangle(const Races::Drivers::Simulation::PositionInTissue& lower_corner, 
                  const Races::Drivers::Simulation::AxisSize& x_size, 
                  const Races::Drivers::Simulation::AxisSize& y_size);

  TissueRectangle(const std::vector<uint16_t>& lower_corner, 
                  const std::vector<uint16_t>& upper_corner);

  TissueRectangle(const std::vector<uint16_t>& lower_corner, 
                  const Races::Drivers::Simulation::AxisSize& x_size, 
                  const Races::Drivers::Simulation::AxisSize& y_size);

  IntegerVector get_lower_corner() const
  {
    IntegerVector lcorner(2);

    lcorner[0] = lower_corner.x;
    lcorner[1] = lower_corner.y;

    return lcorner;
  }

  IntegerVector get_upper_corner() const
  {
    IntegerVector ucorner(2);

    ucorner[0] = upper_corner.x;
    ucorner[1] = upper_corner.y;

    return ucorner;
  }

  void show() const
  {
    Rcout << "TissueRectangle("
          << "(" << lower_corner.x <<"," << lower_corner.y << "),"
          << "(" << upper_corner.x <<"," << upper_corner.y << "))" << std::endl;
  }
};

//' @name TissueRectangle$new
//' @title Build a new rectangle of tissue.
//' @examples
//' # build the rectangle [500,550]x[450,475]
//' rect <- new(TissueRectangle, c(500, 450), c(550, 475))
//'
//' rect
//'
//' # build the rectangle [500,550]x[450,475]
//' rect <- new(TissueRectangle, c(500, 450), 50, 25)
//'
//' rect
TissueRectangle::TissueRectangle(const Races::Drivers::Simulation::PositionInTissue& lower_corner, 
                                 const Races::Drivers::Simulation::PositionInTissue& upper_corner):
  RectangleSet(lower_corner, upper_corner)
{}

TissueRectangle::TissueRectangle(const Races::Drivers::Simulation::PositionInTissue& lower_corner, 
                                 const Races::Drivers::Simulation::AxisSize& x_size, 
                                 const Races::Drivers::Simulation::AxisSize& y_size):
  RectangleSet(lower_corner, x_size, y_size)
{}

TissueRectangle::TissueRectangle(const std::vector<uint16_t>& lower_corner, const std::vector<uint16_t>& upper_corner):
  TissueRectangle(Races::Drivers::Simulation::PositionInTissue{lower_corner[0],lower_corner[1]},
                  Races::Drivers::Simulation::PositionInTissue{upper_corner[0], upper_corner[1]})
{}

TissueRectangle::TissueRectangle(const std::vector<uint16_t>& lower_corner, 
                                 const Races::Drivers::Simulation::AxisSize& x_size, 
                                 const Races::Drivers::Simulation::AxisSize& y_size):
  TissueRectangle(Races::Drivers::Simulation::PositionInTissue{lower_corner[0],lower_corner[1]},
                  x_size, y_size)
{}

//' @name TissueRectangle$lower_corner
//' @title The lower corner of the tissue rectangle.
//' @examples
//' rect <- new(TissueRectangle, c(500, 500), c(550, 550))
//'
//' # get the simulation death activation level
//' rect$lower_corner

//' @name TissueRectangle$upper_corner
//' @title The lower corner of the tissue rectangle.
//' @examples
//' rect <- new(TissueRectangle, c(500, 500), c(550, 550))
//'
//' # get the simulation death activation level
//' rect$upper_corner



//' @name Simulation
//' @title Simulates the cell evolution on a tissue
//' @description The objects of this class can simulate the evolution
//'   of many cells belonging to different *species* on a tissue. Each
//'   cell can duplicate or die according to the rates that delineate
//'   the cell species.
//'
//'   `Simulation` supports epigenetic evolutions, and it lets users
//'   define species pairs that have the same genotype (even though,
//'   its genomic characterization is unknown) and differ because
//'   of their epigenetic state (i.e., either "+" or "-").
//'
//'   `Simulation` models epigenetic mutations and allows a cell in
//'   one of a genotype species to generate a new cell belonging to
//'   the other species of the same genotype at a specified rate.
//'
//'   `Simulation` also allows users to schedule mutations from one
//'   genotype to a different genotype.
//' @field add_genotype Adds a genotype and its species \itemize{
//' \item \emph{Parameter:} \code{genotype} - The genotype name.
//' \item \emph{Parameter:} \code{epigenetic_rates} - The epigenetic rates of the genotype species (optional).
//' \item \emph{Parameter:} \code{growth_rates} - The duplication rates of the genotype species.
//' \item \emph{Parameter:} \code{death_rates} - The death rates of the genotype species.
//' }
//' @field choose_cell_in Chooses one cell in a genotype \itemize{
//' \item \emph{Parameter:} \code{genotype} - The genotype of the cell to choose.
//' \item \emph{Parameter:} \code{lower_corner} - The lower left corner of a rectangular selection (optional).
//' \item \emph{Parameter:} \code{upper_corner} - The upper right corner of a rectangular selection (optional).
//' \item \emph{Returns:} A list reporting "cell_id", "genotype", "epistate", "position_x",
//'    and "position_y" of the choosen cell.
//' }
//' @field death_activation_level The number of cells that activates cell death in a species.
//' @field duplicate_internal_cells Enable/disable duplication for internal cells.
//' @field get_added_cells Gets the cells manually added to the simulation \itemize{
//' \item \emph{Returns:} A data frame reporting "genotype", "epistate", "position_x",
//'         "position_y", and "time" for each cells manually added to
//'         the simulation.
//' }
//' @field search_sample Seach a rectangular sample having a minimum number of cells\itemize{
//' \item \emph{Parameter:} \code{genotype_name} - The genotype of the searched cells.
//' \item \emph{Parameter:} \code{num_of_cells} - The number of cells in the searched sample.
//' \item \emph{Parameter:} \code{width} - The width of the searched sample.
//' \item \emph{Parameter:} \code{height} - The height of the searched sample.
//' \item \emph{Returns:} If a rectangular sample satisfying the provided constraints can 
//'               be found, the corresponding rectangle.
//' }
//' @field get_cell Gets one the tissue cells \itemize{
//' \item \emph{Parameter:} \code{x} - The position of the aimed cell on the x axis.
//' \item \emph{Parameter:} \code{y} - The position of the aimed cell on the y axis.
//' \item \emph{Returns:} A data frame reporting "cell_id", "genotype", "epistate", "position_x",
//'    and "position_y" of the aimed cell.
//' }
//' @field get_cells Gets the tissue cells \itemize{
//' \item \emph{Parameter:} \code{lower_corner} - The lower-left corner of the selection frame (optional).
//' \item \emph{Parameter:} \code{upper_corner} - The upper-right corner of the selection frame (optional).
//' \item \emph{Parameter:} \code{genotype_filter} - The vector of the to-be-selected genotype names (optional).
//' \item \emph{Parameter:} \code{epigenetic_filter} - The vector of the to-be-selected epigenetic states (optional).
//' \item \emph{Returns:} A data frame reporting "cell_id", "genotype", "epistate", "position_x",
//'    and "position_y" for each cells satisfying the provided filters and laying
//'    in the input frame.
//' }
//' @field get_clock Gets the simulated time \itemize{
//' \item \emph{Returns:} The time simulated by the simulation.
//' }
//' @field get_count_history Gets the history of the number of cells per species \itemize{
//' \item \emph{Returns:} A data frame reporting "genotype", "epistate", "counts",
//'     and "time" for each species and for each sampled time.
//' }
//' @field get_counts Counts the number of cells \itemize{
//' \item \emph{Returns:} A data frame reporting "genotype", "epistate", "counts" for each
//'      species in the simulation.
//' }
//' @field get_firing_history Gets the history of the number of fired events \itemize{
//' \item \emph{Returns:} A data frame reporting "event", "genotype", "epistate", "fired",
//'      and "time" for each event type, for each species, and for each sampled time.
//' }
//' @field get_firings Gets the number of fired events \itemize{
//' \item \emph{Returns:} A data frame reporting "event", "genotype", "epistate", and "fired"
//'     for each event type and for each species.
//' }
//' @field get_name Gets the simulation name \itemize{
//' \item \emph{Returns:} The simulation name, which corresponds to the name of the directory
//'         in which the simulation is saving its progresses.
//' }
//' @field get_lineage_graph Gets the simulation lineage graph\itemize{
//' \item \emph{Returns:} A data frame reporting "ancestor", "progeny", and "first_occurrence"
//'         of each species-to-species transition.
//' }
//' @field get_rates Gets the rates of a species\itemize{
//' \item \emph{Parameter:} \code{species} - The species whose rates are aimed.
//' \item \emph{Returns:} The list of the species names.
//' }
//' @field get_samples_forest Get the samples forest\itemize{
//' \item \emph{Returns:} The descendants forest having as leaves the sampled cells.
//' }
//' @field get_samples_info Retrieve information about the samples \itemize{
//' \item \emph{Returns:} A data frame containing, for each sample collected
//'         during the simulation, the columns "name", "time", "ymin",
//'         "xmin", "ymax", "xmax", and  "tumoral cells". "ymin",
//'         "xmin", "ymax", "xmax" report the boundaries of the sampled
//'         rectangular region, while "tumoral cells" is the number of
//'         tumoral cells in the sample.
//' }
//' @field get_species Gets the species \itemize{
//' \item \emph{Returns:} A data frame describing the registered species.
//' }
//' @field get_tissue_name Gets the tissue name \itemize{
//' \item \emph{Returns:} The name of the simulated tissue.
//' }
//' @field get_tissue_size Gets the size of the simulated tissue \itemize{
//' \item \emph{Returns:} The vector `c(x_size, y_size)` of the simulated tissue.
//' }
//' @field mutate_progeny Generate a mutated offspring \itemize{
//' \item \emph{Parameter:} \code{cell_position} - The position of the cell whose offspring will mutate.
//' \item \emph{Parameter:} \code{mutated_genotype} - The genotype of the mutated cell.
//' }
//' or
//' \itemize{
//' \item \emph{Parameter:} \code{x} - The position of the cell whose progeny will mutate on the x axis.
//' \item \emph{Parameter:} \code{y} - The position of the cell whose progeny will mutate on the y axis.
//' \item \emph{Parameter:} \code{mutated_genotype} - The genotype of the mutated cell.
//' }
//' @field place_cell Place one cell in the tissue \itemize{
//' \item \emph{Parameter:} \code{species} - The name of the new cell species.
//' \item \emph{Parameter:} \code{x} - The position on the x axis of the cell.
//' \item \emph{Parameter:} \code{y} - The position on the y axis of the cell.
//' }
//' @field schedule_genotype_mutation Schedules a genotype mutation \itemize{
//' \item \emph{Parameter:} \code{src} - The name of the genotype from which the mutation occurs.
//' \item \emph{Parameter:} \code{dest} - The name of the genotype to which the mutation leads.
//' \item \emph{Parameter:} \code{time} - The simulated time at which the mutation will occurs.
//' }
//' @field run_up_to_event Simulates cell evolution \itemize{
//' \item \emph{Parameter:} \code{event} - The considered event type, i.e., "growth", "death", or "switch".
//' \item \emph{Parameter:} \code{species} - The species whose event number is considered.
//' \item \emph{Parameter:} \code{num_of_events} - The threshold for the event number.
//' }
//' @field run_up_to_size Simulates cell evolution \itemize{
//' \item \emph{Parameter:} \code{species} - The species whose number of cells is considered.
//' \item \emph{Parameter:} \code{num_of_cells} - The threshold for the cell number.
//' }
//' @field run_up_to_time Simulates cell evolution \itemize{
//' \item \emph{Parameter:} \code{time} - The final simulation time.
//' }
//' @field sample_cells Sample a tissue rectangle region \itemize{
//' \item \emph{Parameter:} \code{name} - The sample name.
//' \item \emph{Parameter:} \code{lower_corner} - The bottom-left corner of the rectangle.
//' \item \emph{Parameter:} \code{upper_corner} - The top-right corner of the rectangle.
//' }
//' @field update_rates Updates the rates of a species\itemize{
//' \item \emph{Parameter:} \code{species} - The species whose rates must be updated.
//' \item \emph{Parameter:} \code{rates} - The list of the rates to be updated.
//' \item \emph{Returns:} The vector of the species names.
//' }
//' @field update_tissue Updates tissue name and size \itemize{
//' \item \emph{Parameter:} \code{name} - The new name of the tissue (optional).
//' \item \emph{Parameter:} \code{width} - The width of the new tissue.
//' \item \emph{Parameter:} \code{height} - The height of the new tissue.
//' }
class Simulation
{
  std::shared_ptr<Races::Drivers::Simulation::Simulation> sim_ptr;  //!< The pointer to a RACES simulation object
  std::string name;      //!< The simulation name
  bool save_snapshots;   //!< A flag to preserve binary dump after object destruction

  void init(const SEXP& sexp);

  static bool has_names(const List& list, std::vector<std::string> aimed_names)
  {
    if (aimed_names.size() != static_cast<size_t>(list.size())) {
      return false;
    }

    for (const std::string& name: aimed_names) {
      if (!list.containsElementNamed(name.c_str())) {
        return false;
      }
    }

    return true;
  }

  static bool has_names_in(const List& list, std::set<std::string> aimed_names)
  {
    if (aimed_names.size() < static_cast<size_t>(list.size())) {
      return false;
    }

    CharacterVector names = wrap(list.names());

    for (size_t i=0; i<static_cast<size_t>(names.size()); ++i) {
      if (aimed_names.count(as<std::string>(names[i]))==0) {
        return false;
      }
    }

    return true;
  }

  List get_cells(const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                 const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner,
                 const std::set<Races::Drivers::SpeciesId> &species_filter,
                 const std::set<std::string> &epigenetic_filter) const
  {
    namespace RS = Races::Drivers::Simulation;

    using namespace Races::Drivers;

    if (lower_corner.size() != 2) {
      ::Rf_error("The lower corner must be a vector having size 2");
    }

    if (upper_corner.size() != 2) {
      ::Rf_error("The upper corner must be a vector having size 2");
    }

    size_t num_of_rows = count_driver_mutated_cells(sim_ptr->tissue(), lower_corner, upper_corner,
                                                    species_filter, epigenetic_filter);

    IntegerVector ids(num_of_rows);
    CharacterVector genotype_names(num_of_rows), epi_states(num_of_rows);
    IntegerVector x_pos(num_of_rows), y_pos(num_of_rows);

    size_t i{0};
    for (auto x=lower_corner[0]; x<=upper_corner[0]; ++x) {
      for (auto y=lower_corner[1]; y<=upper_corner[1]; ++y) {
        auto cell_proxy = sim_ptr->tissue()({x,y});
        if(!cell_proxy.is_wild_type()) {

          const RS::CellInTissue& cell = cell_proxy;

          const auto& species = sim_ptr->tissue().get_species(cell.get_species_id());
          const auto sign_string = get_signature_string(species);

          if (species_filter.count(cell.get_species_id())>0
               && epigenetic_filter.count(sign_string)>0) {

            ids[i] = cell.get_id();
            genotype_names[i] = species.get_genotype_name();

            epi_states[i] = sign_string;

            x_pos[i] = x;
            y_pos[i] = y;

            ++i;
          }
        }
      }
    }

    return DataFrame::create(_["cell_id"]=ids, _["genotype"]=genotype_names,
                             _["epistate"]=epi_states, _["position_x"]=x_pos,
                             _["position_y"]=y_pos);
  }

  List wrap_a_cell(const Races::Drivers::Simulation::CellInTissue& cell) const
  {
    using namespace Races::Drivers;

    const auto& species = sim_ptr->tissue().get_species(cell.get_species_id());

    const auto& genotype_name = sim_ptr->find_genotype_name(species.get_genotype_id());

    auto epistate = GenotypeProperties::signature_to_string(species.get_methylation_signature());

    return DataFrame::create(_["cell_id"]=cell.get_id(), _["genotype"]=genotype_name,
                             _["epistate"]=epistate, _["position_x"]=cell.x,
                             _["position_y"]=cell.y);
  }

public:
  Simulation();

  Simulation(const SEXP& sexp);

  Simulation(const SEXP& first_param, const SEXP& second_param);

  Simulation(const std::string& simulation_name, const int& seed, const bool& save_snapshots);

  ~Simulation();

  void update_tissue(const std::string& name, const uint16_t& width, const uint16_t& height);

  void update_tissue(const uint16_t& width, const uint16_t& height);

  void add_genotype(const std::string& genotype, const List& epigenetic_rates,
                    const List& growth_rates, const List& death_rates);

  void add_genotype(const std::string& genotype, const double& growth_rate, const double& death_rate);

  List add_genotype(const List& prova);

  inline Races::Time get_clock() const;

  void place_cell(const std::string& species_name,
                  const Races::Drivers::Simulation::AxisPosition& x,
                  const Races::Drivers::Simulation::AxisPosition& y);

  size_t count_history_sample_in(const Races::Time& minimum_time,
                                 const Races::Time& maximum_time) const;

  List get_added_cells() const;

  List get_counts() const;

  List get_count_history() const;

  List get_count_history(const Races::Time& minimum_time) const;

  List get_count_history(const Races::Time& minimum_time,
                         const Races::Time& maximum_time) const;

  inline List get_cells() const;

  List get_cell(const Races::Drivers::Simulation::AxisPosition& x,
                const Races::Drivers::Simulation::AxisPosition& y) const;

  List get_cells(const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                 const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner) const;

  List get_cells(const SEXP& first_param, const SEXP& second_param) const;

  List get_cells(const std::vector<std::string>& species_filter,
                 const std::vector<std::string>& epigenetic_filter) const;

  List get_cells(const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                 const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner,
                 const std::vector<std::string>& genotype_filter,
                 const std::vector<std::string>& epigenetic_filter) const;

  List get_lineage_graph() const;

  List get_samples_info() const;

  void schedule_genotype_mutation(const std::string& source, const std::string& destination,
                                  const Races::Time& time);

  void run_up_to_time(const Races::Time& time);

  void run_up_to_size(const std::string& species_name, const size_t& num_of_cells);

  void run_up_to_event(const std::string& event, const std::string& species_name,
                       const size_t& num_of_events);

  void sample_cells(const std::string& sample_name,
                    const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                    const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner) const;

  List get_firings() const;

  List get_firing_history() const;

  List get_firing_history(const Races::Time& minimum_time) const;

  List get_firing_history(const Races::Time& minimum_time,
                          const Races::Time& maximum_time) const;

  List get_species() const;

  inline
  std::string get_name() const;

  inline
  const std::string& get_tissue_name() const;

  IntegerVector get_tissue_size() const;

  List get_rates(const std::string& species_name) const;

  void update_rates(const std::string& species_name, const List& list);

  template<typename CHOOSER, std::enable_if_t<std::is_base_of_v<PlainChooser, CHOOSER>, bool> = true>
  List choose_border_cell_in(CHOOSER& chooser)
  {
    namespace RS = Races::Drivers::Simulation;

    const auto directions = get_possible_directions();

    const RS::Tissue& tissue = sim_ptr->tissue();

    size_t i{0};
    while (++i<1000) {
      const auto& cell = chooser();

      for (const auto& dir: directions) {
        RS::PositionInTissue pos = cell;

        do {
          pos = pos + RS::PositionDelta(dir);
        } while (tissue.is_valid(pos) && tissue(pos).is_wild_type());

        if (!tissue.is_valid(pos)) {
          return wrap_a_cell(cell);
        }
      }
    }

    throw std::domain_error("Missed to find a border cell");
  }

  List choose_border_cell_in(const std::string& genotype_name);

  List choose_border_cell_in(const std::string& genotype_name,
                             const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                             const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner);

  List choose_cell_in(const std::string& genotype_name);

  List choose_cell_in(const std::string& genotype_name,
                      const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                      const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner);

  void mutate_progeny(const Races::Drivers::Simulation::AxisPosition& x,
                      const Races::Drivers::Simulation::AxisPosition& y,
                      const std::string& mutated_genotype);

  void mutate_progeny(const List& cell_position, const std::string& mutated_genotype);

  size_t get_death_activation_level() const
  {
    return sim_ptr->death_activation_level;
  }

  void set_death_activation_level(const size_t death_activation_level)
  {
    sim_ptr->death_activation_level = death_activation_level;
  }

  bool get_duplicate_internal_cells() const
  {
    return sim_ptr->duplicate_internal_cells;
  }

  void set_duplicate_internal_cells(const bool duplicate_internal_cells)
  {
    sim_ptr->duplicate_internal_cells = duplicate_internal_cells;
  }

  Races::Time get_history_delta() const
  {
    return sim_ptr->get_statistics().get_history_delta();
  }

  void set_history_delta(const Races::Time history_time_delta)
  {
    sim_ptr->get_statistics().set_history_delta(history_time_delta);
  }

  static Simulation load(const std::string& directory_name)
  {
    using namespace Races::Drivers::Simulation;

    Simulation simulation;

    simulation.save_snapshots = true;
    simulation.name = directory_name;

    auto snapshot_path = BinaryLogger::find_last_snapshot_in(directory_name);

    Races::Archive::Binary::In archive(snapshot_path);

    archive & *(simulation.sim_ptr);

    return simulation;
  }

  SamplesForest get_samples_forest() const;

  TissueRectangle search_sample(const std::string& genotype_name, const size_t& num_of_cells,
                                const uint16_t& width, const uint16_t& height);
};

std::string get_time_string()
{
    std::time_t time;
    std::tm* info;
    char buffer[81];

    std::time(&time);
    info = std::localtime(&time);

    std::strftime(buffer,80,"%Y%m%d-%H%M%S",info);

    return buffer;
}

inline std::string
get_default_name()
{
  return "races_"+get_time_string();
}

inline std::filesystem::path
get_tmp_path()
{
  using namespace std::filesystem;
  size_t i{0};
  std::string base_path = temp_directory_path()/get_default_name();
  auto tmp_path = base_path;

  while (exists(tmp_path)) {
    tmp_path = base_path + "_" + std::to_string(++i);
  }

  return tmp_path;
}

void Simulation::init(const SEXP& sexp)
{
  namespace RS = Races::Drivers::Simulation;

  switch (TYPEOF(sexp)) {
    case INTSXP:
    case REALSXP:
    {
      int seed = as<int>(sexp);
      name = get_default_name();

      if (save_snapshots) {
        sim_ptr = std::make_shared<RS::Simulation>(name, seed);
      } else {
        sim_ptr = std::make_shared<RS::Simulation>(get_tmp_path(), seed);
      }
      break;
    }
    case STRSXP: {
      name = as<std::string>(sexp);

      if (save_snapshots) {
        sim_ptr = std::make_shared<RS::Simulation>(name);
      } else {
        sim_ptr = std::make_shared<RS::Simulation>(get_tmp_path());
      }
      break;
    }
    default: {
      std::ostringstream oss;

      oss << "Invalid type for the first parameter: " 
          << type2name(sexp);

      throw std::domain_error(oss.str());
    }
  }
}

Simulation::Simulation():
  sim_ptr(std::make_shared<Races::Drivers::Simulation::Simulation>(get_tmp_path())),
  name(get_default_name()), save_snapshots(false)
{}

Simulation::Simulation(const SEXP& sexp):
  save_snapshots(false)
{
  if (TYPEOF(sexp) == LGLSXP) {
    save_snapshots = as<bool>(sexp);
    name = get_default_name();

    if (save_snapshots) {
      sim_ptr = std::make_shared<Races::Drivers::Simulation::Simulation>(name);
    } else {
      sim_ptr = std::make_shared<Races::Drivers::Simulation::Simulation>(get_tmp_path());
    }

    return;
  }

  init(sexp);
}

Simulation::Simulation(const SEXP& first_param, const SEXP& second_param):
  save_snapshots(false)
{
  if (TYPEOF(second_param) == LGLSXP) {
    save_snapshots = as<bool>(second_param);

    init(first_param);

    return;
  }

  if (TYPEOF(first_param) != STRSXP) {
    std::ostringstream oss;

    oss << "Invalid type for the parameter 1: " 
        << type2name(first_param)
        << ". If the last parameter is not a Boolean value (save on disk"
        << " parameter), it must be a string (the name of the simulation).";

    throw std::domain_error(oss.str());
  }

  if (TYPEOF(second_param) != INTSXP && TYPEOF(second_param) != REALSXP) {
    std::ostringstream oss;

    oss << "Invalid type for the parameter 2: " 
        << type2name(second_param)
        << ". If the last parameter is not a Boolean value (save on disk"
        << " parameter), it must be an integer value (the random seed).";

    throw std::domain_error(oss.str());
  }

  name = as<std::string>(first_param);
  int seed = as<int>(second_param);

  sim_ptr = std::make_shared<Races::Drivers::Simulation::Simulation>(get_tmp_path(), seed);
}

//' @name Simulation$new
//' @title Constructs a new Simulation
//' @param simulation_name The name of the simulation (optional).
//' @param seed The seed for the pseudo-random generator (optional).
//' @param save_snapshots A flag to save simulation snapshots on disk (optional, 
//'                default `FALSE`).
//' @examples
//' # create a Simulation object storing binary dump in a temporary directory. 
//' # The data are deleted from the disk as soon as the object is destroyed.
//' sim <- new(Simulation, "test")
//'
//' # add a new species, place a cell in the tissue, and let the simulation evolve.
//' sim$add_genotype(genotype = "A", growth_rate = 0.3, death_rate = 0.02)
//' sim$place_cell("A", 500, 500)
//' sim$run_up_to_time(30)
//'
//' # no directory "test" has been created
//' "test" %in% list.files(".")
//'
//' # (let us delete the directory "test" manually)
//' unlink("test", recursive = TRUE)
//' 
//' # By using the optional parameter `save_snapshots`, we force the 
//' # simulation to save its progresses in a local directory whose name 
//' # is the name of the simulation, i.e., "test". This data will be 
//' # preserved when the simulation object will be destroyed.
//' sim <- new(Simulation, "test", save_snapshots=TRUE)
//'
//' # as done above, we add a new species, place a cell in the tissue, and let the 
//' # simulation evolve.
//' sim$add_genotype(genotype = "A", growth_rate = 0.3, death_rate = 0.02)
//' sim$place_cell("A", 500, 500)
//' sim$run_up_to_time(30)
//'
//' # the directory "test" exists and contains a binary dump of 
//' # sthe simulation.
//' "test" %in% list.files(".")
//'
//' # let us manually delete the "test" directory
//' unlink("test", recursive=TRUE)
//'
//' # we can also provide a random seed to the simulation...
//' sim <- new(Simulation, "test", 13)
//'
//' # ...or creating a simulation without providing any name. By default, the 
//' # simulation name will have the following format `races_<date>_<hour>`.
//' sim <- new(Simulation, 13)
Simulation::Simulation(const std::string& simulation_name, const int& seed, const bool& save_snapshots):
  name(simulation_name), save_snapshots(save_snapshots)
{
  if (save_snapshots) {
    sim_ptr = std::make_shared<Races::Drivers::Simulation::Simulation>(simulation_name, seed);
  } else {
    sim_ptr = std::make_shared<Races::Drivers::Simulation::Simulation>(get_tmp_path(), seed);
  }
}

Simulation::~Simulation()
{
  if (sim_ptr.use_count()==1 && !save_snapshots) {
    auto dir = sim_ptr->get_logger().get_directory();

    sim_ptr = std::shared_ptr<Races::Drivers::Simulation::Simulation>();

    std::filesystem::remove_all(dir);
  }
}

//' @name Simulation$update_tissue
//' @title Update tissue name and size
//' @param name The new name of the tissue (optional).
//' @param width The width of the new tissue.
//' @param height The height of the new tissue.
//' @examples
//' sim <- new(Simulation)
//'
//' # set the tissue size, but not the name
//' sim$update_tissue(1200, 900)
//'
//' # set the tissue size and its name
//' sim$update_tissue("Liver", 1200, 900)
void Simulation::update_tissue(const std::string& name,
                               const Races::Drivers::Simulation::AxisSize& width,
                               const Races::Drivers::Simulation::AxisSize& height)
{
  sim_ptr->set_tissue(name, {width, height});
}

void Simulation::update_tissue(const Races::Drivers::Simulation::AxisSize& width,
                               const Races::Drivers::Simulation::AxisSize& height)
{
  sim_ptr->set_tissue("A tissue", {width, height});
}

//' @name Simulation$add_genotype
//' @title Adds a genotype and its species
//' @description This method adds a genotype and its species to the
//'      simulation. If the optional parameter `epigenetic_rate` is
//'      provided, then two new species having the same genotype and
//'      opposite epigenetic states are created. When, instead, the
//'      optional parameter `epigenetic_rate` is missing, this
//'      method creates only one species with no epigenetic states.
//' @param genotype The genotype name.
//' @param epigenetic_rates The epigenetic rates of the genotype species (optional).
//' @param growth_rates The duplication rates of the genotype species.
//' @param death_rates The death rates of the genotype species.
//' @examples
//' sim <- new(Simulation)
//'
//' # create the two species "A+" and "A-". They both have genotype "A".
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//'
//' # create the species "C" its genotype is "C".
//' sim$add_genotype(genotype = "C", growth_rate = 0.2, death_rate = 0.1)
void Simulation::add_genotype(const std::string& genotype, const List& epigenetic_rates,
                              const List& growth_rates, const List& death_rates)
{
  using namespace Races::Drivers;

  if (genotype == "Wild-type") {
    ::Rf_error("\"Wild-type\" is a reserved genotype name.");
  }

  if (!has_names(epigenetic_rates, {"+-","-+"})) {
    ::Rf_error("The second parameter must be a list specifying "
                "the epigenetic rate for \"+-\" and \"-+\"");
  }

  if (!has_names_in(growth_rates, {"+","-"})) {
    ::Rf_error("The third parameter must be a list specifying "
                "the growth rate for \"+\" and \"-\"");
  }

  if (!has_names_in(death_rates, {"+","-"})) {
    ::Rf_error("The fourth parameter must be a list specifying "
                "the death rate for \"+\" and \"-\"");
  }

  GenotypeProperties real_genotype(genotype, {{epigenetic_rates["-+"],epigenetic_rates["+-"]}});

  for (const std::string states: {"+","-"}) {
    if (growth_rates.containsElementNamed(states.c_str())) {
      real_genotype[states].set_rate(CellEventType::DUPLICATION, as<double>(growth_rates[states]));
    }
    if (death_rates.containsElementNamed(states.c_str())) {
      real_genotype[states].set_rate(CellEventType::DEATH, as<double>(death_rates[states]));
    }
  }

  sim_ptr->add_genotype(real_genotype);
}

void Simulation::add_genotype(const std::string& genotype, const double& growth_rate,
                              const double& death_rate)
{
  using namespace Races::Drivers;

  if (genotype == "Wild-type") {
    ::Rf_error("\"Wild-type\" is a reserved genotype name.");
  }

  GenotypeProperties real_genotype(genotype, {});

  real_genotype[""].set_rate(CellEventType::DUPLICATION, growth_rate);
  real_genotype[""].set_rate(CellEventType::DEATH, death_rate);

  sim_ptr->add_genotype(real_genotype);
}

//' @name Simulation$get_species
//' @title Gets the species
//' @return A data frame reporting "genotype", "epistate", "growth_rate",
//'    "death_rate", and "switch_rate" for each registered species.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype("A", growth_rate = 0.2, death_rate = 0.1)
//' sim$add_genotype("B", growth_rate = 0.15, death_rate = 0.05)
//'
//' # get the added species and their rates. In this case, "A"
//' # and "B"
//' sim$get_species()
List Simulation::get_species() const
{
  size_t num_of_rows = sim_ptr->tissue().num_of_species();

  CharacterVector genotype_names(num_of_rows), epi_states(num_of_rows);
  NumericVector switch_rates(num_of_rows), duplication_rates(num_of_rows),
                death_rates(num_of_rows);

  using namespace Races::Drivers;

  size_t i{0};
  for (const auto& species: sim_ptr->tissue()) {
    genotype_names[i] = species.get_genotype_name();
    duplication_rates[i] = species.get_rate(CellEventType::DUPLICATION);
    death_rates[i] = species.get_rate(CellEventType::DEATH);
    epi_states[i] = get_signature_string(species);

    const auto& species_switch_rates = species.get_epigenetic_switch_rates();
    switch(species_switch_rates.size()) {
      case 0:
        switch_rates[i] = NA_REAL;
        break;
      case 1:
        switch_rates[i] = species_switch_rates.begin()->second;
        break;
      default:
        ::Rf_error("rRACES does not support multiple promoters");
    }

    ++i;
  }

  return DataFrame::create(_["genotype"]=genotype_names, _["epistate"]=epi_states,
                            _["growth_rate"]=duplication_rates,
                            _["death_rate"]=death_rates,
                            _["switch_rate"]=switch_rates);
}

//' @name Simulation$place_cell
//' @title Place one cell in the tissue
//' @param species The name of the new cell species.
//' @param x The position on the x axis of the cell.
//' @param y The position on the y axis of the cell.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//'
//' # add into the tissue a cell of species "A+" in position (500,500)
//' sim$place_cell("A+", 500, 500)
void Simulation::place_cell(const std::string& species_name,
                            const Races::Drivers::Simulation::AxisPosition& x,
                            const Races::Drivers::Simulation::AxisPosition& y)
{
  if (sim_ptr->tissue().num_of_mutated_cells()>0) {
    warning("Warning: the tissue already contains a cell.");
  }

  const auto& species = sim_ptr->tissue().get_species(species_name);

  sim_ptr->place_cell(species.get_id(), {x,y});
}

List Simulation::get_cells() const
{
  namespace RS = Races::Drivers::Simulation;

  std::vector<RS::AxisPosition> upper_corner = sim_ptr->tissue().size();
  upper_corner.resize(2);

  for (auto& value : upper_corner) {
    --value;
  }

  return get_cells({0,0}, upper_corner);
}

//' @name Simulation$get_cell
//' @title Gets one of the tissue cells
//' @description This method collects some data of the aimed cell without altering
//'      the tissue.
//' @param x The position of the aimed cell on the x axis.
//' @param y The position of the aimed cell on the y axis.
//' @return A data frame reporting "cell_id", "genotype", "epistate", "position_x",
//'    and "position_y" of the aimed cell.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.02, "-" = 0.01))
//' sim$add_genotype(genotype = "B",
//'                  epigenetic_rates = c("+-" = 0.02, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.3, "-" = 0.1),
//'                  death_rates = c("+" = 0.02, "-" = 0.01))
//' sim$schedule_genotype_mutation(src = "A", dst = "B", time = 50)
//' sim$place_cell("A+", 500, 500)
//' sim$run_up_to_time(40)
//'
//' # collect all the cells in the tissue
//' sim$get_cell(501, 502)
List Simulation::get_cell(const Races::Drivers::Simulation::AxisPosition& x,
                          const Races::Drivers::Simulation::AxisPosition& y) const
{
  namespace RS = Races::Drivers::Simulation;

  const RS::CellInTissue& cell = sim_ptr->tissue()({x,y});

  return wrap_a_cell(cell);
}

List Simulation::get_cells(const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                           const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner) const
{
  std::set<Races::Drivers::SpeciesId> species_ids;

  for (const auto& species: sim_ptr->tissue()) {
    species_ids.insert(species.get_id());
  }

  return get_cells(lower_corner, upper_corner, species_ids, {"+", "-", ""});
}

List Simulation::get_cells(const SEXP& first_param, const SEXP& second_param) const
{
  using namespace Races::Drivers::Simulation;

  if (TYPEOF(first_param)!=TYPEOF(second_param)) {
    std::ostringstream oss;

    oss << "The two parameters have different types: " << type2name(first_param)
        << " != " << type2name(second_param);

    throw std::domain_error(oss.str());
  }

  switch (TYPEOF(first_param)) {
      case INTSXP:
      case REALSXP:
      {
          return get_cells(as<std::vector<AxisPosition>>(first_param),
                            as<std::vector<AxisPosition>>(second_param));
      }
      case STRSXP: {
          return get_cells(as<std::vector<std::string>>(first_param),
                            as<std::vector<std::string>>(second_param));
      }
      default: {
        std::ostringstream oss;

        oss << "Invalid parameter type " << type2name(first_param);

        throw std::domain_error(oss.str());
      }
  }
}

List Simulation::get_cells(const std::vector<std::string>& species_filter,
                           const std::vector<std::string>& epigenetic_filter) const
{
  namespace RS = Races::Drivers::Simulation;

  std::vector<RS::AxisPosition> upper_corner = sim_ptr->tissue().size();
  upper_corner.resize(2);

  for (auto& value : upper_corner) {
    --value;
  }

  return get_cells({0,0}, upper_corner, species_filter, epigenetic_filter);
}

//' @name Simulation$get_cells
//' @title Gets the tissue cells
//' @description This method collects some data about the cells in the tissue
//'      without altering the tissue itself. The pairs of optional parameters
//'      `lower_corner` and `upper_corner` define a frame of the tissue in
//'      which the data are sampled. The optional parameters `genotype_filter`
//'      and `epigenetic_filter` filter the collected cell data according to
//'      the cell genotype and epigenetic state.
//' @param lower_corner The lower-left corner of the selection frame (optional).
//' @param upper_corner The upper-right corner of the selection frame (optional).
//' @param genotype_filter The vector of the to-be-selected genotype names (optional).
//' @param epigenetic_filter The vector of the to-be-selected epigenetic states (optional).
//' @return A data frame reporting "cell_id", "genotype", "epistate", "position_x",
//'    and "position_y" for each cells satisfying the provided filters and laying
//'    in the input frame.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$add_genotype(genotype = "B",
//'                  epigenetic_rates = c("+-" = 0.02, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.3, "-" = 0.1),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$schedule_genotype_mutation(src = "A", dst = "B", time = 50)
//' sim$place_cell("A+", 500, 500)
//' sim$run_up_to_time(30)
//'
//' # collect all the cells in the tissue
//' sim$get_cells()
//'
//' # get the cells in the frame [495,505]x[490,500]
//' sim$get_cells(lower_corner=c(495,490), upper_corner=c(505,500))
//'
//' # cells can be filtered by genotype name...
//' sim$get_cells(genotype_filter=c("A"),epigenetic_filter=c("+","-"))
//'
//' # ...or by epigenetic state
//' sim$get_cells(genotype_filter=c("A","B"),epigenetic_filter=c("-"))
//'
//' # cells can be filtered by frame, genotype, and epigenetic states
//' sim$get_cells(lower_corner=c(495,495), upper_corner=c(505,505),
//'               genotype_filter=c("A"),epigenetic_filter=c("+","-"))
List Simulation::get_cells(const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                           const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner,
                           const std::vector<std::string>& genotype_filter,
                           const std::vector<std::string>& epigenetic_filter) const
{
  std::set<std::string> genotype_set(genotype_filter.begin(), genotype_filter.end());
  std::set<std::string> epigenetic_set(epigenetic_filter.begin(), epigenetic_filter.end());

  auto species_ids = get_species_ids_from_genotype_name(sim_ptr->tissue(), genotype_set);

  return get_cells(lower_corner, upper_corner, species_ids, epigenetic_set);
}

//' @name Simulation$get_counts
//' @title Counts the number of cells
//' @return A data frame reporting "genotype", "epistate", "counts" for each
//'      species in the simulation.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype("A", growth_rate = 0.2, death_rate = 0.1)
//' sim$add_genotype("B", growth_rate = 0.15, death_rate = 0.05)
//' sim$schedule_genotype_mutation(src = "A", dst = "B", time = 50)
//' sim$place_cell("A", 500, 500)
//' sim$run_up_to_time(70)
//'
//' # counts the number of cells per species
//' sim$get_counts()
List Simulation::get_counts() const
{
  using namespace Races::Drivers;

  size_t num_of_rows = sim_ptr->tissue().num_of_species();

  CharacterVector genotype_names(num_of_rows);
  CharacterVector epi_states(num_of_rows);
  IntegerVector counts(num_of_rows);

  size_t i{0};
  for (const auto& species: sim_ptr->tissue()) {
    genotype_names[i] = species.get_genotype_name();
    epi_states[i] = get_signature_string(species);
    counts[i] = species.num_of_cells();
    ++i;
  }

  return DataFrame::create(_["genotype"]=genotype_names, _["epistate"]=epi_states,
                            _["counts"]=counts);
}

std::map<Races::Drivers::SpeciesId, std::string>
get_species_id2name(const Races::Drivers::Simulation::Tissue& tissue)
{
  std::map<Races::Drivers::SpeciesId, std::string> id2name;
  for (const auto& species : tissue) {
    id2name[species.get_id()] = species.get_name();
  }

  return id2name;
}

//' @name Simulation$get_added_cells
//' @title Gets the cells manually added to the simulation
//' @return A data frame reporting "genotype", "epistate", "position_x",
//'         "position_y", and "time" for each cells manually added to
//'         the simulation.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$add_genotype(genotype = "B",
//'                  epigenetic_rates = c("+-" = 0.02, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.3, "-" = 0.1),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$schedule_genotype_mutation(src = "A", dst = "B", time = 30)
//' sim$place_cell("A+", 500, 500)
//' sim$run_up_to_time(50)
//'
//' # counts the number of cells per species
//' sim$get_added_cells()
List Simulation::get_added_cells() const
{
  using namespace Races::Drivers;

  namespace RS = Races::Drivers::Simulation;

  size_t num_of_rows = sim_ptr->get_added_cells().size();

  CharacterVector genotype_names(num_of_rows),  epi_states(num_of_rows);
  IntegerVector position_x(num_of_rows), position_y(num_of_rows);
  NumericVector time(num_of_rows);

  size_t i{0};
  for (const auto& added_cell: sim_ptr->get_added_cells()) {
    const auto& species = sim_ptr->tissue().get_species(added_cell.species_id);
    genotype_names[i] = sim_ptr->find_genotype_name(species.get_genotype_id());
    epi_states[i] = get_signature_string(species);
    position_x[i] = added_cell.x;
    position_y[i] = added_cell.y;
    time[i] = added_cell.time;
    ++i;
  }

  return DataFrame::create(_["genotype"]=genotype_names, _["epistate"]=epi_states,
                           _["position_x"]=position_x,  _["position_y"]=position_y,
                           _["time"] = time);
}

//' @name Simulation$schedule_genotype_mutation
//' @title Schedules a genotype mutation
//' @description This method schedules a genotype mutation that can occur
//'      from any of the species of the source genotype to the species of
//'      the destination genotype with a consistent epigenetic state.
//'      For the sake of example, if the mutation from "A" to "B" is
//'      scheduled, then we have three possible situations:
//'      1. The genotype "A" consists of the only species "A". Then,
//'         during one duplication of a cell of "A", one cell of "B"
//'         will arise.
//'      2. The genotype "A" consists of the species "A+" and "A-" and
//'         during one duplication of a cell of "A+", one cell of "B+"
//'         will arise.
//'      3. The genotype "A" consists of the species "A+" and "A-" and
//'         during one duplication of a cell of "A-", one cell of "B-"
//'         will arise.
//'      No other scenario can occur.
//' @param src The name of the genotype from which the mutation occurs.
//' @param dest The name of the genotype to which the mutation leads.
//' @param time The simulated time at which the mutation will occurs.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$add_genotype(genotype = "B",
//'                  epigenetic_rates = c("+-" = 0.02, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.3, "-" = 0.1),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//'
//' # schedule an evolution from genotype "A" to genotype "B" at time 50
//' sim$schedule_genotype_mutation(src = "A", dst = "B", time = 50)
void Simulation::schedule_genotype_mutation(const std::string& src, const std::string& dest,
                                            const Races::Time& time)
{
  sim_ptr->schedule_genotype_mutation(src,dest,time);
}

// sorting LineageEdge by time
struct TimedLineageEdge : public Races::Drivers::Simulation::LineageEdge
{
  Races::Time time;

  TimedLineageEdge():
    Races::Drivers::Simulation::LineageEdge(), time(0)
  {}

  TimedLineageEdge(const Races::Drivers::Simulation::LineageEdge& edge, const Races::Time& time):
    Races::Drivers::Simulation::LineageEdge(edge), time(time)
  {}
};

struct TimedLineageEdgeCmp
{
  bool operator()(const TimedLineageEdge& a, const TimedLineageEdge& b)
  {
    return (a.time<b.time
            || (a.time==b.time && (a.get_ancestor()<b.get_ancestor()))
            || (a.time==b.time && (a.get_ancestor()==b.get_ancestor())
                && (a.get_progeny()<b.get_progeny())));
  }
};

std::vector<TimedLineageEdge> sorted_timed_edges(const Races::Drivers::Simulation::Simulation& simulation)
{
  const auto& lineage_graph = simulation.get_lineage_graph();
  const size_t num_of_edges = lineage_graph.num_of_edges();

  std::vector<TimedLineageEdge> timed_edges;

  timed_edges.reserve(num_of_edges);

  for (const auto& [edge, edge_time] : lineage_graph) {
    timed_edges.push_back({edge, edge_time});
  }

  TimedLineageEdgeCmp cmp;
  sort(timed_edges.begin(), timed_edges.end(), cmp);

  return timed_edges;
}

//' @name Simulation$get_lineage_graph
//' @title Gets the simulation lineage graph
//' @description At the beginning of the computation only the species of the added
//'         cells are present in the tissue. As the simulation proceeds new species
//'         arise as a consequence of either genotype mutations or epigenetic
//'         switches. The *lineage graph* stores these species evolutions and it
//'         reports the first occurrence time of any species-to-species transition.
//'
//'         This method returns the lineage graph of the simulation.
//' @return A data frame reporting "ancestor", "progeny", and "first_occurrence" of
//'         each species-to-species transition.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$add_genotype(genotype = "B",
//'                  epigenetic_rates = c("+-" = 0.02, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.3, "-" = 0.1),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$schedule_genotype_mutation(src = "A", dst = "B", time = 20)
//' sim$run_up_to_time(50)
//'
//' sim$get_lineage_graph()
List Simulation::get_lineage_graph() const
{
  const auto species_id2name = get_species_id2name(sim_ptr->tissue());

  const auto timed_edges = sorted_timed_edges(*sim_ptr);

  CharacterVector ancestors(timed_edges.size()), progeny(timed_edges.size());
  NumericVector first_cross(timed_edges.size());

  size_t i{0};
  for (const auto& timed_edge : timed_edges) {
    ancestors[i] = (timed_edge.get_ancestor() != WILD_TYPE_SPECIES ?
                    species_id2name.at(timed_edge.get_ancestor()):
                    "Wild-type");

    progeny[i] = (timed_edge.get_progeny() != WILD_TYPE_SPECIES ?
                  species_id2name.at(timed_edge.get_progeny()):
                  "Wild-type");
    first_cross[i] = timed_edge.time;

    ++i;
  }

  return DataFrame::create(_["ancestor"]=ancestors, _["progeny"]=progeny,
                            _["first_cross"]=first_cross);
}

inline void validate_non_empty_tissue(const Races::Drivers::Simulation::Tissue& tissue)
{
  if (tissue.num_of_cells()==0) {
    ::Rf_error("The tissue does not contain any cell.");
  }
}

//' @name Simulation$run_up_to_time
//' @title Simulates cell evolution
//' @param time The final simulation time.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype("A", growth_rate = 0.2, death_rate = 0.1)
//' sim$place_cell("A", 500, 500)
//'
//' # simulate the tissue up to simulate timed 100
//' sim$run_up_to_time(40)
void Simulation::run_up_to_time(const Races::Time& time)
{
  validate_non_empty_tissue(sim_ptr->tissue());

  Races::UI::ProgressBar bar;

  RTest<Races::Drivers::Simulation::TimeTest> ending_test{time};

  sim_ptr->run(ending_test, bar);
}

//' @name Simulation$run_up_to_size
//' @title Simulates cell evolution
//' @description This method simulates cell evolution until the number of cells in
//'       a species reaches a specified threshold.
//' @param species The species whose number of cells is considered.
//' @param num_of_cells The threshold for the cell number.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$place_cell("A+", 500, 500)
//'
//' # simulate the tissue until the species "A+" account for 100
//' # contemporary cells
//' sim$run_up_to_size(species = "A+", num_of_cells = 100)
void Simulation::run_up_to_size(const std::string& species_name, const size_t& num_of_cells)
{
  Races::UI::ProgressBar bar;

  validate_non_empty_tissue(sim_ptr->tissue());

  const auto& species_id = sim_ptr->tissue().get_species(species_name).get_id();

  RTest<Races::Drivers::Simulation::SpeciesCountTest> ending_test{species_id, num_of_cells};

  sim_ptr->run(ending_test, bar);
}

//' @name Simulation$run_up_to_event
//' @title Simulates cell evolution
//' @description This method simulates cell evolution until the number of events that
//'         have occurred to cells of a species reaches a specified threshold.
//' @param event The considered event, i.e., "growth", "death", or "switch".
//' @param species The species whose event number is considered.
//' @param num_of_events The threshold for the event number.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$place_cell("A+", 500, 500)
//'
//' # simulate the cell evolution until the number of epigenetic events from
//' # the species "A+" is less than 100.
//' sim$run_up_to_event(event = "switch", species = "A+", num_of_events = 100)
void Simulation::run_up_to_event(const std::string& event, const std::string& species_name,
                                 const size_t& num_of_events)
{
  Races::UI::ProgressBar bar;

  validate_non_empty_tissue(sim_ptr->tissue());

  if (event_names.count(event)==0) {
    handle_unknown_event(event);
  }

  namespace RS = Races::Drivers::Simulation;

  const auto& species_id = sim_ptr->tissue().get_species(species_name).get_id();

  RTest<RS::EventCountTest> ending_test{event_names.at(event), species_id, num_of_events};

  sim_ptr->run(ending_test, bar);
}

//' @name Simulation$get_clock
//' @title Gets the simulated time
//' @return The time simulated by the simulation.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$place_cell("A+", 500, 500)
//' sim$run_up_to_event("switch", "A+", 100)
//'
//' # get the simulated time
//' sim$get_clock()
Races::Time Simulation::get_clock() const
{
  return sim_ptr->get_time();
}

//' @name Simulation$get_firings
//' @title Gets the number of fired events
//' @return A data frame reporting "event", "genotype", "epistate", and "fired"
//'     for each event type, genotype, and epigenetic states.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$place_cell("A+", 500, 500)
//' sim$run_up_to_event("switch", "A+", 100)
//'
//' # get the number of event fired per event and species
//' sim$get_firings()
List Simulation::get_firings() const
{
  const auto last_time_sample = sim_ptr->get_statistics().get_last_time_in_history();

  auto df = get_firing_history(last_time_sample, last_time_sample);

  return DataFrame::create(_["event"]=df["event"], _["genotype"]=df["genotype"],
                           _["epistate"]=df["epistate"], _["fired"]=df["fired"]);
}

//' @name Simulation$get_firing_history
//' @title Gets the history of the number of fired events
//' @description This method returns a data frame reporting the number of
//'           events fired up to each sampled simulation time.
//' @return A data frame reporting "event", "genotype", "epistate", "fired",
//'     and "time" for each event type, for each species, and for each
//'     sampled time.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$place_cell("A+", 500, 500)
//' sim$history_delta <- 20
//' sim$run_up_to_time(70)
//'
//' # get the number of event fired per event and species
//' sim$get_firing_history()
List Simulation::get_firing_history() const
{
  return get_firing_history(0);
}

List Simulation::get_firing_history(const Races::Time& minimum_time) const
{
  if (sim_ptr->get_statistics().get_history().size()==0) {
    return get_firing_history(0,0);
  }

  const auto last_time_sample = sim_ptr->get_statistics().get_last_time_in_history();

  return get_firing_history(minimum_time, last_time_sample);
}

size_t Simulation::count_history_sample_in(const Races::Time& minimum_time,
                                           const Races::Time& maximum_time) const
{
  size_t num_of_samples{0};
  const auto& history = sim_ptr->get_statistics().get_history();
  auto series_it = history.lower_bound(minimum_time);
  while (series_it != history.end()
         && series_it->first <= maximum_time) {
      ++num_of_samples;
      ++series_it;
  }

  return num_of_samples;
}

List Simulation::get_firing_history(const Races::Time& minimum_time,
                                    const Races::Time& maximum_time) const
{
  //using namespace Races::Drivers;

  const size_t rows_per_sample = event_names.size()*sim_ptr->tissue().num_of_species();
  const size_t num_of_rows = count_history_sample_in(minimum_time, maximum_time)*rows_per_sample;

  CharacterVector events(num_of_rows), genotype_names(num_of_rows),
                  epi_states(num_of_rows);
  IntegerVector firings(num_of_rows);
  NumericVector times(num_of_rows);

  size_t i{0};
  const auto& history = sim_ptr->get_statistics().get_history();
  auto series_it = history.lower_bound(minimum_time);
  while (series_it != history.end() && series_it->first <= maximum_time) {
    const auto& time = series_it->first;
    const auto& t_stats = series_it->second;
    for (const auto& species: sim_ptr->tissue()) {
      for (const auto& [event_name, event_code]: event_names) {
        events[i] = event_name;
        genotype_names[i] = species.get_genotype_name();
        epi_states[i] = get_signature_string(species);

        const auto& species_it = t_stats.find(species.get_id());
        if (species_it != t_stats.end()) {
          firings[i] = count_events(species_it->second, event_code);
        } else {
          firings[i] = 0;
        }
        times[i] = time;
        ++i;
      }
    }
    ++series_it;
  }

  return DataFrame::create(_["event"]=events, _["genotype"]=genotype_names,
                           _["epistate"]=epi_states, _["fired"]=firings,
                           _["time"]=times);
}

//' @name Simulation$get_count_history
//' @title Gets the history of the number of cells per species
//' @description This method returns a data frame reporting the number of
//'           species cells in each sampled simulation time.
//' @return A data frame reporting "genotype", "epistate", "counts",
//'     and "time" for each species, and for each sampled time.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype("A", growth_rate = 0.2, death_rate = 0.1)
//' sim$add_genotype("B", growth_rate = 0.15, death_rate = 0.05)
//' sim$schedule_genotype_mutation(src = "A", dst = "B", time = 50)
//' sim$place_cell("A", 500, 500)
//' sim$history_delta <- 20
//' sim$run_up_to_time(70)
//'
//' # get the history of species counts
//' sim$get_count_history()
List Simulation::get_count_history() const
{
  return get_count_history(0);
}

List Simulation::get_count_history(const Races::Time& minimum_time) const
{
  if (sim_ptr->get_statistics().get_history().size()==0) {
    return get_count_history(0,0);
  }

  const auto last_time_sample = sim_ptr->get_statistics().get_last_time_in_history();

  return get_count_history(minimum_time, last_time_sample);
}

List Simulation::get_count_history(const Races::Time& minimum_time,
                                   const Races::Time& maximum_time) const
{
  const size_t rows_per_sample = sim_ptr->tissue().num_of_species();
  const size_t num_of_rows = count_history_sample_in(minimum_time, maximum_time)*rows_per_sample;

  CharacterVector genotype_names(num_of_rows), epi_states(num_of_rows);
  IntegerVector counts(num_of_rows);
  NumericVector times(num_of_rows);

  size_t i{0};
  const auto& history = sim_ptr->get_statistics().get_history();
  auto series_it = history.lower_bound(minimum_time);
  while (series_it != history.end() && series_it->first <= maximum_time) {
    const auto& time = series_it->first;
    const auto& t_stats = series_it->second;
    for (const auto& species: sim_ptr->tissue()) {
      genotype_names[i] = species.get_genotype_name();
      epi_states[i] = get_signature_string(species);

      const auto& species_it = t_stats.find(species.get_id());
      if (species_it != t_stats.end()) {
        counts[i] = species_it->second.curr_cells;
      } else {
        counts[i] = 0;
      }
      times[i] = time;
      ++i;
    }
    ++series_it;
  }

  return DataFrame::create(_["genotype"]=genotype_names, _["epistate"]=epi_states,
                           _["count"]=counts, _["time"]=times);
}

//' @name Simulation$get_name
//' @title Gets the simulation name
//' @return The simulation name, which corresponds to the name of the directory
//'         in which the simulation is saving its progresses.
//' @examples
//' sim <- new(Simulation)
//'
//' # Expecting "test"
//' sim$get_name()
std::string Simulation::get_name() const
{
  return name;
}

//' @name Simulation$get_tissue_name
//' @title Gets the tissue name
//' @return The name of the simulated tissue.
//' @examples
//' sim <- new(Simulation)
//' sim$update_tissue("Liver", 1200, 900)
//'
//' # get the tissue name, i.e., expecting "Liver"
//' sim$get_tissue_name()
const std::string& Simulation::get_tissue_name() const
{
  return sim_ptr->tissue().get_name();
}

//' @name Simulation$get_tissue_size
//' @title Gets the size of the simulated tissue
//' @return The vector `c(x_size, y_size)` of the simulated tissue.
//' @examples
//' sim <- new(Simulation)
//' sim$update_tissue("Liver", 1200, 900)
//'
//' # get the tissue size, i.e., expecting c(1200,900)
//' sim$get_tissue_size()
IntegerVector Simulation::get_tissue_size() const
{
  auto size_vect = sim_ptr->tissue().size();

  return {size_vect[0], size_vect[1]};
}

//' @name Simulation$get_rates
//' @title Get the rates of a species
//' @param species The species whose rates are aimed.
//' @return The list of the species rates.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.02),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//'
//' # Get the rates of "A-". In this case c("growth"=0.08, "death"=0.01, "switch"=0.02) is expected
//' sim$get_rates("A-")
List Simulation::get_rates(const std::string& species_name) const
{
  auto& species = sim_ptr->tissue().get_species(species_name);

  List rates = List::create(_("growth") = species.get_rate(event_names.at("growth")),
                            _["death"] = species.get_rate(event_names.at("death")));

  if (species.get_methylation_signature().size()>0) {
    rates.push_back(species.get_rate(event_names.at("switch")),"switch");
  }

  return rates;
}

//' @name Simulation$update_rates
//' @title Update the rates of a species
//' @param species The species whose rates must be updated.
//' @param rates The list of rates to be updated.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//'
//' # Set the death and epigenetic switch rates of "A-" to 0
//' sim$update_rates("A-", c(switch=0, death=0))
void Simulation::update_rates(const std::string& species_name, const List& rates)
{
  using namespace Races::Drivers;
  auto& species = sim_ptr->tissue().get_species(species_name);

  if (!rates.hasAttribute("names")) {
    throw std::domain_error("update_rates: The second parameter must be a list "
                            "with the names attribute");
  }

  Rcpp::CharacterVector nv = rates.names();
  for (int i=0; i<nv.size(); i++) {
    auto event_name = as<std::string>(nv[i]);
    auto event_it = event_names.find(event_name);
    if (event_it == event_names.end()) {
      handle_unknown_event(event_name);
    }
    species.set_rate(event_it->second, as<double>(rates[i]));
  }
}

//' @name Simulation$choose_cell_in
//' @title Chooses one cell in a genotype
//' @description This method chooses one of the cells whose genotype
//'         is `genotype`. Optionally, the lower and upper corners
//'         of a tissue rectangular selection can be provided
//'         to obtain one cell in the rectangle.
//' @param genotype The genotype of the cell to choose.
//' @param lower_corner The lower corner of the rectangular selection (optional).
//' @param upper_corner The upper corner of the rectangular selection (optional).
//' @return A list reporting "cell_id", "genotype", "epistate", "position_x",
//'    and "position_y" of the choosen cell.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$add_genotype(genotype = "B",
//'                  epigenetic_rates = c("+-" = 0.1, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.15, "-" = 0.3),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//' sim$place_cell("A+", 500, 500)
//' sim$death_activation_level <- 100
//' sim$schedule_genotype_mutation("A","B",20)
//' sim$run_up_to_size(species = "B-", num_of_cells = 50)
//'
//' # Randomly choose one cell in "B" in the tissue
//' sim$choose_cell_in(genotype = "B")
List Simulation::choose_cell_in(const std::string& genotype_name,
                                const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                                const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner)
{
  namespace RS = Races::Drivers::Simulation;

  if (sim_ptr->duplicate_internal_cells) {
    auto rectangle = get_rectangle(lower_corner, upper_corner);
    const auto& cell = sim_ptr->choose_cell_in(genotype_name, rectangle,
                                               Races::Drivers::CellEventType::DUPLICATION);

    return wrap_a_cell(cell);
  }

  return choose_border_cell_in(genotype_name, lower_corner, upper_corner);
}

List Simulation::choose_cell_in(const std::string& genotype_name)
{
  namespace RS = Races::Drivers::Simulation;

  if (sim_ptr->duplicate_internal_cells) {
    const auto& cell = sim_ptr->choose_cell_in(genotype_name,
                                                Races::Drivers::CellEventType::DUPLICATION);
    return wrap_a_cell(cell);
  }

  return choose_border_cell_in(genotype_name);
}

List Simulation::choose_border_cell_in(const std::string& genotype_name)
{
  PlainChooser chooser(sim_ptr, genotype_name);

  return choose_border_cell_in(chooser);
}

List Simulation::choose_border_cell_in(const std::string& genotype_name,
                                       const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                                       const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner)
{
  RectangularChooser chooser(sim_ptr, genotype_name, lower_corner, upper_corner);

  return choose_border_cell_in(chooser);
}

void Simulation::mutate_progeny(const Races::Drivers::Simulation::AxisPosition& x,
                                const Races::Drivers::Simulation::AxisPosition& y,
                                const std::string& mutated_genotype)
{
  auto pos_in_tissue = get_position_in_tissue({x,y});

  namespace RS = Races::Drivers::Simulation;

  sim_ptr->simulate_genotype_mutation(pos_in_tissue, mutated_genotype);
}

//' @name Simulation$mutate_progeny
//' @title Generate a mutated progeny
//' @description This method simulates both the duplication of the cell in the
//'       specified position and the birth of one cells of a given
//'       genotype that preserves the epigenetic status of the original cell.
//'       The mutated cell will be located in the position of its parent.
//' @param cell_position The position of the cell whose offspring will mutate.
//' @param mutated_genotype The genotype of the mutated cell.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  epigenetic_rates = c("+-" = 0.01, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.2, "-" = 0.08),
//'                  death_rates = c("+" = 0.01, "-" = 0.01))
//' sim$place_cell("A+", 500, 500)
//' sim$run_up_to_time(30)
//'
//' sim$add_genotype(genotype = "B",
//'                  epigenetic_rates = c("+-" = 0.1, "-+" = 0.01),
//'                  growth_rates = c("+" = 0.15, "-" = 0.3),
//'                  death_rates = c("+" = 0.1, "-" = 0.01))
//'
//' # duplicate the cell in position (503, 492). One of
//' # its direct descendents will have genotype "B"
//' sim$mutate_progeny(503, 492, "B")
//'
//' # the output of `choose_cell_in` and `get_cell` can also be used
//' # as input for `mutate_progeny`
//' sim$mutate_progeny(sim$choose_cell_in("A"), "B")
void Simulation::mutate_progeny(const List& cell_position,
                                const std::string& mutated_genotype)
{
  namespace RS = Races::Drivers::Simulation;

  std::vector<Races::Drivers::Simulation::AxisPosition> vector_position;

  for (const std::string axis : {"x", "y"}) {
    auto field = "position_"+axis;
    if (!cell_position.containsElementNamed(field.c_str())) {
      std::string msg = "Missing \"" + field + "\" element from the list.";

      ::Rf_error(msg.c_str());
    }
    vector_position.push_back(as<RS::AxisPosition>(cell_position[field]));
  }

  return mutate_progeny(vector_position[0], vector_position[1], mutated_genotype);
}

//' @name Simulation$sample_cells
//' @title Sample a tissue rectangle region.
//' @description This method removes a rectangular region from the simulated
//'       tissue and stores its cells in a sample that can subsequently
//'       retrieved to build a samples forest.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  growth_rate = 0.2,
//'                  death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//'
//' sim$death_activation_level <- 100
//' sim$run_up_to_size(species = "A", num_of_cells = 50000)
//'
//' # sample the region [450,500]x[475,550]
//' sim$sample_cells("S1", lower_corner=c(450,475), upper_corner=c(500,550))
void Simulation::sample_cells(const std::string& sample_name,
                              const std::vector<Races::Drivers::Simulation::AxisPosition>& lower_corner,
                              const std::vector<Races::Drivers::Simulation::AxisPosition>& upper_corner) const
{
  using namespace Races::Drivers;

  auto rectangle = get_rectangle(lower_corner, upper_corner);

  sim_ptr->sample_tissue(sample_name, rectangle);
}

template<typename SAMPLES>
List get_samples_info(const SAMPLES& samples)
{
  CharacterVector sample_name(samples.size());
  NumericVector time(samples.size());
  IntegerVector ymin(samples.size()), ymax(samples.size()),
                xmin(samples.size()), xmax(samples.size()),
                non_wild(samples.size());

  size_t i{0};
  for (const auto& sample : samples) {
    sample_name[i] = sample.get_name();
    time[i] = sample.get_time();
    non_wild[i] = sample.get_cell_ids().size();

    const auto& rectangle = sample.get_region();
    xmin[i] = rectangle.lower_corner.x;
    xmax[i] = rectangle.upper_corner.x;
    ymin[i] = rectangle.lower_corner.y;
    ymax[i] = rectangle.upper_corner.y;

    ++i;
  }

  return DataFrame::create(_["name"]=sample_name, _["xmin"]=xmin,
                           _["ymin"]=ymin, _["xmax"]=xmax,
                           _["ymax"]=ymax,
                           _["tumoural cells"]=non_wild,
                           _["time"]=time);
}

//' @name Simulation$get_samples_info
//' @title Retrieve information about the samples
//' @description This method retrieves information about
//'           the samples collected along the simulation.
//'           It returns a data frame reporting, for each
//'           sample, the name, the sampling time, the
//'           position, and the number of tumoural cells.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  growth_rate = 0.2,
//'                  death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//'
//' sim$death_activation_level <- 100
//' sim$run_up_to_size(species = "A", num_of_cells = 50000)
//'
//' # sample the region [450,500]x[475,550]
//' sim$sample_cells("S1", lower_corner=c(450,475),
//'                  upper_corner=c(500,550))
//'
//' # simulate 1 time unit more
//' sim$run_up_to_time(sim$get_clock()+1)
//'
//' # sample the region [500,520]x[525,550]
//' sim$sample_cells("S2", lower_corner=c(500,525),
//'                  upper_corner=c(520,550))
//'
//' # get information about all the collected
//' # samples, i.e, S1 and S2
//' sim$get_samples_info()
List Simulation::get_samples_info() const
{
  return ::get_samples_info(sim_ptr->get_tissue_samples());
}

//' @name Simulation$death_activation_level
//' @title The number of cells that activates cell death in a species.
//' @description This value is the minimum number of cells that
//'       enables cell death in a species. The cell of a species $S$ can die
//'       if and only if that $S$ has reached the death activation level at
//'       least once during the simulation.
//' @examples
//' sim <- new(Simulation)
//'
//' # get the simulation death activation level
//' sim$death_activation_level
//'
//' # set the death activation level to 50
//' sim$death_activation_level <- 50


//' @name Simulation$duplicate_internal_cells
//' @title Enable/disable duplication for internal cells.
//' @description This Boolean flag enable/disable duplication of internal
//'            cells. When it is set to `FALSE`, the border-growth model
//'            is used. Otherwise, the homogeneous-growth model is applied.
//'            It is set to `FALSE` by default.
//' @examples
//' sim <- new(Simulation)
//'
//' # is the duplication of internal cells enabled? (by default, no)
//' sim$duplicate_internal_cells
//'
//' # enable homogeneous-growth model
//' sim$duplicate_internal_cells <- TRUE
//'
//' # now it should be set to `TRUE`
//' sim$duplicate_internal_cells
//'
//' # enable boder-growth model
//' sim$duplicate_internal_cells <- FALSE

//' @name Simulation$history_delta
//' @title The delta time between time series samples
//' @description This value is the maximum time between two successive
//'          time series data samples.
//' @examples
//' sim <- new(Simulation)
//'
//' # get the delta time between two time series samples (0 by default)
//' sim$history_delta
//'
//' # set the delta time between two time series samples
//' sim$death_activation_level <- 20


//' @name recover_simulation
//' @title Load a simulation
//' @param name The name of the simulation to be recovered
//' @examples
//' # create a simulation having name "recover_simulation_test" and
//' # save its snapshots in a local directory
//' sim <- new(Simulation, "recover_simulation_test",
//'            save_snapshots=TRUE)
//'
//' # add the species of "A"
//' sim$add_genotype("A",
//'                  epigenetic_rates=c("+-" = 0.01, "-+"=0.01),
//'                  growth_rates = c("+"=0.1, "-"=0.01),
//'                  death_rates = c("+"=0.05, "-"=0.005))
//'
//' # place a cell in the tissue
//' sim$place_cell("A+", 500, 500)
//'
//' # simulate up to time 50
//' sim$run_up_to_time(50)
//'
//' # show the simulation
//' sim
//'
//' # remove the object sim from the environment
//' rm(list=c("sim"))
//'
//' # the object pointed by sim does not exist any more
//' exists("sim")
//'
//' # recover the simulation from the directory "recover_simulation_test"
//' sim <- recover_simulation("recover_simulation_test")
//'
//' sim
//'
//' # delete dump directory
//' unlink("recover_simulation_test", recursive=TRUE)


//' @name SamplesForest
//' @title The forest of the sampled cell ancestors.
//' @description Represents the forest of the ancestors of the
//'       cells sampled during the computation. The leaves of
//'       this forest are the sampled cells.
//' @field get_coalescent_cells Retrieve most recent common ancestors\itemize{
//' \item \emph{Parameter:} \code{cell_ids} - The list of the identifiers of the
//'               cells whose most recent common ancestors are aimed (optional).
//' \item \emph{Return:} A data frame representing, for each of the identified
//'         cells, the identified (column "cell_id"), whenever the
//'         node is not a root, the ancestor identifier (column
//'         "ancestor"), whenever the node was sampled, i.e., it is
//'         one of the forest leaves, the name of the sample
//'         containing the node, (column "sample"), the genotype
//'         (column "genotype"), the epistate (column "epistate"),
//'         and the birth time (column "birth_time").
//' }
//' @field get_nodes Get the forest nodes \itemize{
//' \item \emph{Return:} A data frame representing, for each node
//'              in the forest, the identified (column "id"),
//'              whenever the node is not a root, the ancestor
//'              identifier (column "ancestor"), whenever the node
//'              was sampled, i.e., it is one of the forest
//'              leaves, the name of the sample containing the
//'              node, (column "sample"), the genotype (column
//'              "genotype"), the epistate (column "epistate"),
//'              and the birth time (column "birth_time").
//' }
//' @field get_samples_info Retrieve information about the samples \itemize{
//' \item \emph{Returns:} A data frame containing, for each sample collected
//'         during the simulation, the columns "name", "time", "ymin",
//'         "xmin", "ymax", "xmax", and  "tumoral cells". "ymin",
//'         "xmin", "ymax", "xmax" report the boundaries of the sampled
//'         rectangular region, while "tumoral cells" is the number of
//'         tumoral cells in the sample.
//' }
//' @field get_species_info Gets the species data\itemize{
//' \item \emph{Returns:} A data frame reporting "genotype" and "epistate"
//'            for each registered species.
//' }
//' @field get_subforest_for Build a subforest using as leaves some of the original samples \itemize{
//' \item \emph{Parameter:} \code{sample_names} - The names of the samples whose cells will be used
//'         as leaves of the new forest.
//' \item \emph{Returns:} A samples forest built on the samples mentioned in `sample_names`.
//' }
class SamplesForest : private Races::Drivers::DescendantsForest
{
  SamplesForest();

  List get_nodes(const std::vector<Races::Drivers::CellId>& cell_ids) const;

public:
  SamplesForest(const Races::Drivers::Simulation::Simulation& simulation);

  List get_nodes() const;

  List get_samples_info() const;

  List get_species_info() const;

  List get_coalescent_cells() const;

  List get_coalescent_cells(const std::list<Races::Drivers::CellId>& cell_ids) const;

  SamplesForest get_subforest_for(const std::vector<std::string>& sample_names) const;

  void show() const;
};

SamplesForest::SamplesForest():
  Races::Drivers::DescendantsForest()
{}

// This method produces a segmentation fault and I cannot understand why
/*
SamplesForest::SamplesForest(const Simulation& simulation):
  Races::Drivers::DescendantsForest(*(simulation.sim_ptr))
{}
*/

SamplesForest::SamplesForest(const Races::Drivers::Simulation::Simulation& simulation):
  Races::Drivers::DescendantsForest(simulation)
{}

//' @name SamplesForest$get_nodes
//' @title Get the nodes of the forest
//' @return A data frame representing, for each node
//'         in the forest, the identified (column "cell_id"),
//'         whenever the node is not a root, the ancestor
//'         identifier (column "ancestor"), whenever the
//'         node was sampled, i.e., it is one of the forest
//'         leaves, the name of the sample containing the
//'         node, (column "sample"), the genotype (column
//'         "genotype"), the epistate (column "epistate"),
//'         and the birth time (column "birth_time").
//' @examples
//' # create a simulation having name "get_nodes_test"
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  growth_rate = 0.2,
//'                  death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//'
//' sim$death_activation_level <- 100
//' sim$run_up_to_size(species = "A", num_of_cells = 50000)
//'
//' # sample the region [450,500]x[475,550]
//' sim$sample_cells("S1", lower_corner=c(450,475), upper_corner=c(500,550))
//'
//' # build the samples forest
//' forest <- sim$get_samples_forest()
//'
//' forest$get_nodes()
List SamplesForest::get_nodes() const
{
  std::vector<Races::Drivers::CellId> cell_ids;
  cell_ids.reserve(num_of_nodes());
  for (const auto& [cell_id, cell]: get_cells()) {
    cell_ids.push_back(cell_id);
  }

  return get_nodes(cell_ids);
}

List SamplesForest::get_nodes(const std::vector<Races::Drivers::CellId>& cell_ids) const
{
  using namespace Races::Drivers;

  IntegerVector ids(cell_ids.size()), ancestors(cell_ids.size());
  CharacterVector genotypes(cell_ids.size()), epi_states(cell_ids.size()),
                  sample_names(cell_ids.size());
  NumericVector birth(cell_ids.size());

  size_t i{0};
  for (const auto& cell_id: cell_ids) {
    ids[i] = cell_id;
    auto cell_node = get_node(cell_id);
    if (cell_node.is_root()) {
      ancestors[i] = NA_INTEGER;
    } else {
      ancestors[i] = cell_node.parent().get_id();
    }

    genotypes[i] = cell_node.get_genotype_name();
    epi_states[i] = GenotypeProperties::signature_to_string(cell_node.get_methylation_signature());

    if (cell_node.is_leaf()) {
      sample_names[i] = cell_node.get_sample().get_name();
    } else {
      sample_names[i] = NA_STRING;
    }
    birth[i] = static_cast<const Cell&>(cell_node).get_birth_time();

    ++i;
  }

  return DataFrame::create(_["cell_id"]=ids, _["ancestor"]=ancestors,
                           _["genotype"]=genotypes, _["epistate"]=epi_states,
                           _["sample"]=sample_names, _["birth_time"]=birth);
}

//' @name SamplesForest$get_samples_info
//' @title Retrieve information about the samples
//' @description This method retrieves information about
//'           the samples whose cells were used as leaves
//'           of the samples forest.
//' @return A data frame reporting, for each sample, the
//'           name, the sampling time, the position, and
//'           the number of tumoural cells.
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  growth_rate = 0.2,
//'                  death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//'
//' sim$death_activation_level <- 100
//' sim$run_up_to_size(species = "A", num_of_cells = 50000)
//'
//' # sample the region [450,500]x[475,550]
//' sim$sample_cells("S1", lower_corner=c(450,475), upper_corner=c(500,550))
//'
//' # build the samples forest
//' forest <- sim$get_samples_forest()
//'
//' # get information about the sampled whose cells
//' # are the forest leaves, i.e, S1 and S2
//' forest$get_samples_info()
List SamplesForest::get_samples_info() const
{
  return ::get_samples_info(get_samples());
}

List SamplesForest::get_coalescent_cells() const
{
  auto coalencent_ids = Races::Drivers::DescendantsForest::get_coalescent_cells();

  return get_nodes(coalencent_ids);
}

//' @name SamplesForest$get_coalescent_cells
//' @title Retrieve most recent common ancestors
//' @description This method retrieves the most recent common ancestors
//'         of a set of cells. If the optional parameter `cell_ids` is
//'         used, this method find the most recent common ancestors of
//'         the cells having an identifier among those in `cell_ids`.
//'         If, otherwise, the optional parameter is not used, this
//'         method find the most recent common ancestors of the forest
//'         leaves.
//' @param cell_ids The list of the identifiers of the cells whose
//'         most recent common ancestors are aimed (optional).
//' @return A data frame representing, for each of the identified
//'         cells, the identified (column "cell_id"), whenever the
//'         node is not a root, the ancestor identifier (column
//'         "ancestor"), whenever the node was sampled, i.e., it is
//'         one of the forest leaves, the name of the sample
//'         containing the node, (column "sample"), the genotype
//'         (column "genotype"), the epistate (column "epistate"),
//'         and the birth time (column "birth_time").
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  growth_rate = 0.2,
//'                  death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//'
//' sim$death_activation_level <- 100
//' sim$run_up_to_size(species = "A", num_of_cells = 50000)
//'
//' # sample the region [450,500]x[475,550]
//' sim$sample_cells("S1", lower_corner=c(450,475), upper_corner=c(500,550))
//'
//' # build the samples forest
//' forest <- sim$get_samples_forest()
//'
//' forest$get_coalescent_cells()
List SamplesForest::get_coalescent_cells(const std::list<Races::Drivers::CellId>& cell_ids) const
{
  auto coalencent_ids = Races::Drivers::DescendantsForest::get_coalescent_cells(cell_ids);

  return get_nodes(coalencent_ids);
}

//' @name SamplesForest$get_subforest_for
//' @title Build a subforest using as leaves some of the original samples
//' @param sample_names The names of the samples whose cells will be used
//'         as leaves of the new forest
//' @return A samples forest built on the samples mentioned in `sample_names`
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  growth_rate = 0.2,
//'                  death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//'
//' sim$death_activation_level <- 100
//' sim$run_up_to_size(species = "A", num_of_cells = 50000)
//'
//' # sample the region [450,500]x[475,550]
//' sim$sample_cells("S1", lower_corner=c(450,475), upper_corner=c(500,550))
//'
//' sim$run_up_to_size(species = "A", num_of_cells = 60000)
//'
//' # sample again the same region
//' sim$sample_cells("S2", lower_corner=c(450,475), upper_corner=c(500,550))
//'
//' # build the samples forest
//' forest <- sim$get_samples_forest()
//'
//' forest$get_subforest_for("S2")
SamplesForest SamplesForest::get_subforest_for(const std::vector<std::string>& sample_names) const
{
  SamplesForest forest;

  static_cast< Races::Drivers::DescendantsForest&>(forest) = Races::Drivers::DescendantsForest::get_subforest_for(sample_names);

  return forest;
}

//' @name SamplesForest$get_species_info
//' @title Gets the species
//' @return A data frame reporting "genotype" and "epistate"
//'            for each registered species.
List SamplesForest::get_species_info() const
{
  size_t num_of_rows = get_species_data().size();

  CharacterVector genotype_names(num_of_rows), epi_states(num_of_rows);

  using namespace Races::Drivers;

  size_t i{0};
  for (const auto& [species_id, species_data]: get_species_data()) {
    genotype_names[i] = get_genotype_name(species_data.genotype_id);
    epi_states[i] = GenotypeProperties::signature_to_string(species_data.signature);

    ++i;
  }

  return DataFrame::create(_["genotype"]=genotype_names, _["epistate"]=epi_states);
}

void SamplesForest::show() const
{
  size_t num_of_leaves{0};
  for (const auto& sample: get_samples()) {
    num_of_leaves += sample.get_cell_ids().size();
  }

  Rcout << "SamplesForest(# of trees: " << get_roots().size()
        << ", # of nodes: " << num_of_nodes()
        << ", # of leaves: " << num_of_leaves
        << ", samples: {";

  std::string sep = "";
  for (const auto& sample: get_samples()) {
    Rcout << sep << "\"" << sample.get_name() << "\"";
    sep = ", ";
  }

  Rcout << "})" << std::endl;
}

//' @name Simulation$get_samples_forest
//' @title Get the samples forest
//' @return The samples forest having as leaves the sampled cells
//' @examples
//' sim <- new(Simulation)
//' sim$add_genotype(genotype = "A",
//'                  growth_rate = 0.2,
//'                  death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//'
//' sim$death_activation_level <- 100
//' sim$run_up_to_size(species = "A", num_of_cells = 50000)
//'
//' # sample the region [450,500]x[475,550]
//' sim$sample_cells("S1", lower_corner=c(450,475), upper_corner=c(500,550))
//'
//' # build the samples forest
//' forest <- sim$get_samples_forest()
SamplesForest Simulation::get_samples_forest() const
{
  return SamplesForest(*sim_ptr);
}

size_t count_in(const std::set<Races::Drivers::SpeciesId>& species_ids,
                const Races::Drivers::Simulation::Tissue& tissue,
                const uint16_t init_x, const uint16_t init_y,
                const uint16_t& width, const uint16_t& height)
{
  size_t counter{0};
  auto sizes = tissue.size();

  uint16_t x_max = std::min(static_cast<uint16_t>(init_x+width), sizes[0]);
  uint16_t y_max = std::min(static_cast<uint16_t>(init_y+height), sizes[1]);

  for (uint16_t x=init_x; x<x_max; ++x) {
    for (uint16_t y=init_y; y<y_max; ++y) {
      auto cell_proxy = tissue({x,y});
      if (!cell_proxy.is_wild_type()) {
        const Races::Drivers::Simulation::CellInTissue& cell = cell_proxy;

        if (species_ids.count(cell.get_species_id())>0) {
          ++counter;
        }
      }
    }
  }

  return counter;
}

std::set<Races::Drivers::SpeciesId> 
collect_species_of(const Races::Drivers::Simulation::Simulation& simulation,
                   const std::string& genotype_name)
{
  const auto& tissue = simulation.tissue();
  auto genotype_id = simulation.find_genotype_id(genotype_name);

  std::set<Races::Drivers::SpeciesId> species_ids;

  for (const auto& species: tissue) {
    if (species.get_genotype_id()==genotype_id) {
      species_ids.insert(species.get_id());
    }
  }

  return species_ids;
}

//' @name Simulation$search_sample
//' @title Search a rectangular sample containing a minimum number of cells
//' @description This method searches a rectangular tissue sample containing 
//'        the provided number of cells. The sizes of the sample are also
//'        provided a parameter of the method. 
//'        The complexity of this method is O(|tissue rows|*|tissue cols|).
//' @param genotype_name The genotype of the searched cells.
//' @param num_of_cells The number of cells in the searched sample.
//' @param width The width of the searched sample.
//' @param height The height of the searched sample.
//' @return If a rectangular sample satisfying the provided constraints can 
//'               be found, the corresponding rectangle.
//' @examples
//' sim <- new(Simulation)
//' sim$death_activation_level <- 50
//' sim$add_genotype(genotype = "A", growth_rate = 0.2, death_rate = 0.01)
//' sim$place_cell("A", 500, 500)
//' sim$run_up_to_size(species = "A", num_of_cells = 500)
//'
//' sim$add_genotype(genotype = "B", growth_rate = 0.3, death_rate = 0.01)
//' sim$mutate_progeny(sim$choose_cell_in("A"), "B")
//' sim$run_up_to_size(species = "B", num_of_cells = 1000)
//'
//' # find a 10x10 sample containing 80 "B" cells
//' sim$search_sample("B",80,50,50)
TissueRectangle Simulation::search_sample(const std::string& genotype_name, const size_t& num_of_cells,
                                          const uint16_t& width, const uint16_t& height)
{
  auto species_ids = collect_species_of(*sim_ptr, genotype_name);

  const auto& tissue = sim_ptr->tissue();
  const auto tissue_sizes = tissue.size();

  uint16_t grid_width = tissue_sizes[0]/width+((tissue_sizes[0]%width>0?1:0));
  uint16_t grid_height = tissue_sizes[1]/height+((tissue_sizes[1]%height>0?1:0));

  std::vector<size_t> grid_column(grid_height);
  std::vector<std::vector<size_t>> grid(grid_width, grid_column);

  for (uint16_t grid_x=0; grid_x<grid_width; ++grid_x) {
    for (uint16_t grid_y=0; grid_y<grid_height; ++grid_y) {
      const uint16_t x = grid_x*width, y = grid_y*height;
      grid[grid_x][grid_y] = count_in(species_ids, tissue, x, y, 
                                      width, height);
      if (grid[grid_x][grid_y]>num_of_cells) {
        return TissueRectangle(Races::Drivers::Simulation::PositionInTissue{x,y}, width, height);
      }
    }
  }

  throw std::runtime_error("No bounding box found!");
}

namespace RS = Races::Drivers::Simulation;
namespace RD = Races::Drivers;

RCPP_EXPOSED_CLASS(TissueRectangle)
RCPP_EXPOSED_CLASS(Simulation)
RCPP_EXPOSED_CLASS(SamplesForest)
RCPP_MODULE(Drivers){
  class_<TissueRectangle>("TissueRectangle")
  .constructor<std::vector<uint16_t>, std::vector<uint16_t>>("Create a new rectangle")
  .constructor<std::vector<uint16_t>, RS::AxisSize, RS::AxisSize>("Create a new rectangle")
  .property("lower_corner",&TissueRectangle::get_lower_corner, "The rectangle lower corner")
  .property("upper_corner",&TissueRectangle::get_upper_corner, "The rectangle upper corner")
  .method("show",&TissueRectangle::show);

  class_<Simulation>("Simulation")
  .constructor("Create a simulation whose name is \"races_<year>_<hour><minute><second>\"")
  .constructor<SEXP>("Create a simulation")
  .constructor<SEXP, SEXP>("Crete a simulation")
  .constructor<std::string, int, bool>("Crete a simulation: the first parameter is the simulation name; "
                                       "the second one is the random seed; the third one is a Boolean flag "
                                       "to enable/disable simulation saves")

  // place_cell
  .method("place_cell", &Simulation::place_cell, "Place a cell in the tissue")

  // add_genotype
  .method("add_genotype", (void (Simulation::*)(const std::string&, const List&, const List&,
                                                const List&))(&Simulation::add_genotype),
          "Add a new species with epigenetic status")
  .method("add_genotype", (void (Simulation::*)(const std::string&, const double&,
                                                const double&))(&Simulation::add_genotype),
          "Add a new species")

  .method("choose_cell_in", (List (Simulation::*)(const std::string&))(&Simulation::choose_cell_in),
          "Randomly choose one cell in a genotype")

  .method("choose_cell_in", (List (Simulation::*)(const std::string&,
                                                  const std::vector<RS::AxisPosition>&,
                                                  const std::vector<RS::AxisPosition>&))(&Simulation::choose_cell_in),
          "Randomly choose one cell having a specified genotype in a rectangular selection")

  // schedule_genotype_mutation
  .method("schedule_genotype_mutation", &Simulation::schedule_genotype_mutation,
          "Add a timed mutation between two different species")

  // get_species
  .method("get_species", &Simulation::get_species,
          "Get the species added to the simulation")

  // get_samples_forest
  .method("get_samples_forest", &Simulation::get_samples_forest,
          "Get the descendants forest having as leaves the sampled cells")

  // death_activation_level
  .property("death_activation_level", &Simulation::get_death_activation_level,
                                      &Simulation::set_death_activation_level,
            "The number of cells in a species that activates cell death" )

  // duplicate_internal_cells
  .property("duplicate_internal_cells", &Simulation::get_duplicate_internal_cells,
                                        &Simulation::set_duplicate_internal_cells,
            "Enable/disable duplication for internal cells" )

  // get_clock
  .method("get_clock", &Simulation::get_clock, "Get the current simulation time")

  // get_cell
  .method("get_cell", (List (Simulation::*)(const RS::AxisPosition&,
                                            const RS::AxisPosition&) const)(&Simulation::get_cell),
          "Get one cell from the simulated tissue")

  // get_cells
  .method("get_cells", (List (Simulation::*)(const std::vector<RS::AxisPosition>&,
                                             const std::vector<RS::AxisPosition>&,
                                             const std::vector<std::string>&,
                                             const std::vector<std::string>&) const)(&Simulation::get_cells),
          "Get cells from the simulated tissue")
  .method("get_cells", (List (Simulation::*)(const SEXP&, const SEXP&) const)(&Simulation::get_cells),
          "Get cells from the simulated tissue")
  .method("get_cells", (List (Simulation::*)() const)(&Simulation::get_cells),
          "Get cells from the simulated tissue")

  // get_name
  .method("get_name", &Simulation::get_name, "Get the simulation name")

  // get_lineage_graph
  .method("get_lineage_graph", &Simulation::get_lineage_graph,
          "Get the simulation lineage graph")

  // get_tissue_name
  .method("get_tissue_name", &Simulation::get_tissue_name, "Get the simulation tissue name")

  // get_tissue_size
  .method("get_tissue_size", &Simulation::get_tissue_size, "Get the simulation tissue size")

  // get_added_cells
  .method("get_added_cells", &Simulation::get_added_cells,
          "Get the cells manually added to the simulation")

  // get_counts
  .method("get_counts", &Simulation::get_counts, "Get the current number of cells per species")

  // get_count_history
  .method("get_count_history", (List (Simulation::*)() const)&Simulation::get_count_history,
          "Get the number of simulated events per species along the computation")

  // get_firings
  .method("get_firings", &Simulation::get_firings,
          "Get the current number of simulated events per species")

  // get_firing_history
  .method("get_firing_history", (List (Simulation::*)() const)&Simulation::get_firing_history,
          "Get the number of simulated events per species along the computation")

  // get_rates
  .method("get_rates", &Simulation::get_rates,
          "Get the rates of a species")

  // get_samples_info
  .method("get_samples_info", &Simulation::get_samples_info,
          "Get some pieces of information about the collected samples")

  // history_delta
  .property("history_delta", &Simulation::get_history_delta,
                             &Simulation::set_history_delta,
            "The sampling delta for the get_*_history functions" )

  // mutate
  .method("mutate_progeny",  (void (Simulation::*)(const List&, const std::string&))
                                                  (&Simulation::mutate_progeny),
          "Duplicate a cell and mutate one of its children")
  .method("mutate_progeny",  (void (Simulation::*)(const RS::AxisPosition&,
                                                   const RS::AxisPosition&, const std::string&))
                                                  (&Simulation::mutate_progeny),
          "Duplicate a cell and mutate one of its children")

  // run_up_to_time
  .method("run_up_to_time", &Simulation::run_up_to_time,
          "Simulate the system up to the specified simulation time")

  // run_up_to_event
  .method("run_up_to_event", &Simulation::run_up_to_event,
          "Simulate the system up to the specified number of events")

  // run_up_to_size
  .method("run_up_to_size", &Simulation::run_up_to_size,
          "Simulate the system up to the specified number of cells in the species")

  // sample_cells
  .method("sample_cells", &Simulation::sample_cells,
          "Sample a rectangular region of the tissue")

  // update rates
  .method("update_rates", &Simulation::update_rates,
          "Update the rates of a species")

  // update_tissue
  .method("update_tissue", (void (Simulation::*)(const std::string&, const RS::AxisSize&,
                                                 const RS::AxisSize&))(&Simulation::update_tissue),
          "Update tissue name and size")
  .method("update_tissue", (void (Simulation::*)(const RS::AxisSize&,
                                                 const RS::AxisSize&))(&Simulation::update_tissue),
          "Update tissue size")
  .method("search_sample", &Simulation::search_sample, 
          "Search a rectangular sample containing a given number of cells");

  // recover_simulation
  function("recover_simulation", &Simulation::load,
           "Recover a simulation");

  class_<SamplesForest>("SamplesForest")
    // get_nodes
    .method("get_nodes", (List (SamplesForest::*)() const)(&SamplesForest::get_nodes),
            "Get the nodes of the forest")

    // get_coalescent_cells
    .method("get_coalescent_cells",
            (List (SamplesForest::*)(const std::list<Races::Drivers::CellId>&) const)
                (&SamplesForest::get_coalescent_cells),
            "Get the most recent common ancestor of some cells")

    // get_coalescent_cells
    .method("get_coalescent_cells",
            (List (SamplesForest::*)() const)(&SamplesForest::get_coalescent_cells),
            "Get the most recent common ancestor of all the forest trees")

    // get_subforest_for
    .method("get_subforest_for", &SamplesForest::get_subforest_for,
            "Get the sub-forest for some of the original samples")

    // get_samples_info
    .method("get_samples_info", &SamplesForest::get_samples_info,
            "Get some pieces of information about the samples")

    // get_species
    .method("get_species_info", &SamplesForest::get_species_info,
            "Get the recorded species")

    // show
    .method("show", &SamplesForest::show,
            "Describe the SampleForest");
}
