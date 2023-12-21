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

#ifndef __RRACES_PHYLOGENETIC_FOREST__
#define __RRACES_PHYLOGENETIC_FOREST__

#include <vector>

#include <Rcpp.h>

#include <phylogenetic_forest.hpp>

#include "forest.hpp"
#include "snv.hpp"
#include "cna.hpp"

class MutationEngine;

class PhylogeneticForest : private Races::Mutations::PhylogeneticForest
{
  PhylogeneticForest(const Races::Mutations::PhylogeneticForest& orig);
public:
  PhylogeneticForest();

  inline Rcpp::List get_nodes() const
  {
    return ForestCore::get_nodes(static_cast<const Races::Mutations::PhylogeneticForest&>(*this));
  }

  inline Rcpp::List get_samples_info() const
  {
    return ForestCore::get_samples_info(static_cast<const Races::Mutations::PhylogeneticForest&>(*this));
  }

  inline Rcpp::List get_species_info() const
  {
    return ForestCore::get_species_info(static_cast<const Races::Mutations::PhylogeneticForest&>(*this));
  }

  inline Rcpp::List get_coalescent_cells() const
  {
    return ForestCore::get_coalescent_cells(static_cast<const Races::Mutations::PhylogeneticForest&>(*this));
  }

  inline Rcpp::List get_coalescent_cells(const std::list<Races::Mutants::CellId>& cell_ids) const
  {
    return ForestCore::get_coalescent_cells(static_cast<const Races::Mutations::PhylogeneticForest&>(*this), 
                                            cell_ids);
  }

  PhylogeneticForest get_subforest_for(const std::vector<std::string>& sample_names) const;

  Rcpp::List get_sampled_cell_SNVs() const;

  Rcpp::List get_sampled_cell_SNVs(const Races::Mutants::CellId& cell_ids) const;

  Races::Mutants::CellId get_first_occurrence(const SNV& snv) const;

  Races::Mutants::CellId get_first_occurrence(const CNA& cna) const;

  void save(const std::string& filename) const;

  static PhylogeneticForest load(const std::string& filename);

  void show() const;

  friend class MutationEngine;
};

RCPP_EXPOSED_CLASS(PhylogeneticForest)

#endif // __RRACES_PHYLOGENETIC_FOREST__
