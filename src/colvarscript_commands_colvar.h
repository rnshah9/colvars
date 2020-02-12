// -*- c++ -*-

// This file is part of the Collective Variables module (Colvars).
// The original version of Colvars and its updates are located at:
// https://github.com/Colvars/colvars
// Please update all Colvars source files before making any changes.
// If you wish to distribute your changes, please submit them to the
// Colvars repository at GitHub.


CVSCRIPT(colvar_addforce,
         "Apply the given force onto this colvar and return the same",
         1, 1,
         "force : float or array - Applied force; must match colvar dimensionality",
         std::string const f_str(script->obj_to_str(script->get_colvar_cmd_arg(0, objc, objv)));
         std::istringstream is(f_str);
         is.width(cvm::cv_width);
         is.precision(cvm::cv_prec);
         colvarvalue force(this_colvar->value());
         force.is_derivative();
         if (force.from_simple_string(is.str()) != COLVARS_OK) {
           script->add_error_msg("addforce : error parsing force value");
           return COLVARSCRIPT_ERROR;
         }
         this_colvar->add_bias_force(force);
         script->set_result_str(force.to_simple_string());
         return COLVARS_OK;
         )

CVSCRIPT(colvar_cvcflags,
         "Enable or disable individual components by setting their active flags",
         1, 1,
         "flags : integer array - Zero/nonzero value disables/enables the CVC",
         std::string const flags_str(script->obj_to_str(script->get_colvar_cmd_arg(0, objc, objv)));
         std::istringstream is(flags_str);
         std::vector<bool> flags;
         int flag;
         while (is >> flag) {
           flags.push_back(flag != 0);
         }
         int res = this_colvar->set_cvc_flags(flags);
         if (res != COLVARS_OK) {
           script->add_error_msg("Error setting CVC flags");
           return COLVARSCRIPT_ERROR;
         }
         script->set_result_str("0");
         return COLVARS_OK;
         )

CVSCRIPT(colvar_delete,
         "Delete this colvar, along with all biases that depend on it",
         0, 0,
         "",
         delete this_colvar;
         return COLVARS_OK;
         )

CVSCRIPT(colvar_get,
         "Get the value of the given feature for this colvar",
         1, 1,
         "feature : string - Name of the feature",
         return script->proc_features(this_colvar, objc, objv);
         )

CVSCRIPT(colvar_getappliedforce,
         "Return the total of the forces applied to this colvar",
         0, 0,
         "",
         script->set_result_str((this_colvar->applied_force()).to_simple_string());
         return COLVARS_OK;
         )

CVSCRIPT(colvar_getatomgroups,
         "Return the atom indices used by this colvar as a list of lists",
         0, 0,
         "",
         std::string result;
         std::vector<std::vector<int> > lists = this_colvar->get_atom_lists();
         std::vector<std::vector<int> >::iterator li = lists.begin();
         for ( ; li != lists.end(); ++li) {
           result += "{";
           std::vector<int>::iterator lj = (*li).begin();
           for ( ; lj != (*li).end(); ++lj) {
             result += cvm::to_str(*lj);
             result += " ";
           }
           result += "} ";
         }
         script->set_result_str(result);
         return COLVARS_OK;
         )

CVSCRIPT(colvar_getatomids,
         "Return the list of atom indices used by this colvar",
         0, 0,
         "",
         std::string result;
         std::vector<int>::iterator li = this_colvar->atom_ids.begin();
         for ( ; li != this_colvar->atom_ids.end(); ++li) {
           result += cvm::to_str(*li);
           result += " ";
         }
         script->set_result_str(result);
         return COLVARS_OK;
         )

CVSCRIPT(colvar_getconfig,
         "Return the configuration string of this colvar",
         0, 0,
         "",
         script->set_result_str(this_colvar->get_config());
         return COLVARS_OK;
         )

CVSCRIPT(colvar_getgradients,
         "Return the atomic gradients of this colvar",
         0, 0,
         "",
         std::string result;
         std::vector<cvm::rvector>::iterator li =
           this_colvar->atomic_gradients.begin();
         for ( ; li != this_colvar->atomic_gradients.end(); ++li) {
           result += "{";
           int j;
           for (j = 0; j < 3; ++j) {
             result += cvm::to_str((*li)[j]);
             result += " ";
           }
           result += "} ";
         }
         script->set_result_str(result);
         return COLVARS_OK;
         )

CVSCRIPT(colvar_gettotalforce,
         "Return the sum of internal and external forces to this colvar",
         0, 0,
         "",
         script->set_result_str((this_colvar->total_force()).to_simple_string());
         return COLVARS_OK;
         )

CVSCRIPT(colvar_modifycvcs,
         "Modify configuration of individual components by passing string arguments",
         1, 1,
         "confs : sequence of strings - New configurations; empty strings are skipped",
         std::vector<std::string> const confs(script->proxy()->script_obj_to_str_vector(script->get_colvar_cmd_arg(0, objc, objv)));
         cvm::increase_depth();
         int res = this_colvar->update_cvc_config(confs);
         cvm::decrease_depth();
         if (res != COLVARS_OK) {
           script->add_error_msg("Error setting CVC flags");
           return COLVARSCRIPT_ERROR;
         }
         script->set_result_str("0");
         return COLVARS_OK;
         )

CVSCRIPT(colvar_run_ave,
         "Get the current running average of the value of this colvar",
         0, 0,
         "",
         script->set_result_str(this_colvar->run_ave().to_simple_string());
         return COLVARS_OK;
         )

CVSCRIPT(colvar_set,
         "Set the given feature of this colvar to a new value",
         2, 2,
         "feature : string - Name of the feature\n"
         "value : string - String representation of the new feature value",
         return script->proc_features(this_colvar, objc, objv);
         )

CVSCRIPT(colvar_state,
         "Print a string representation of the feature state of this colvar",
         0, 0,
         "",
         this_colvar->print_state();
         return COLVARS_OK;
         )

CVSCRIPT(colvar_type,
         "Get the type description of this colvar",
         0, 0,
         "",
         script->set_result_str(this_colvar->value().type_desc(this_colvar->value().value_type));
         return COLVARS_OK;
         )

CVSCRIPT(colvar_update,
         "Recompute this colvar and return its up-to-date value",
         0, 0,
         "",
         this_colvar->calc();
         this_colvar->update_forces_energy();
         script->set_result_str((this_colvar->value()).to_simple_string());
         return COLVARS_OK;
         )

CVSCRIPT(colvar_value,
         "Get the current value of this colvar",
         0, 0,
         "",
         script->set_result_str(this_colvar->value().to_simple_string());
         return COLVARS_OK;
         )

CVSCRIPT(colvar_width,
         "Get the width of this colvar",
         0, 0,
         "",
         script->set_result_str(cvm::to_str(this_colvar->width, 0,
                                            cvm::cv_prec));
         return COLVARS_OK;
         )