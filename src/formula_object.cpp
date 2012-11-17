#include <map>
#include <string>
#include <stdio.h>

#include <boost/shared_ptr.hpp>

#include "formula.hpp"
#include "formula_object.hpp"
#include "json_parser.hpp"
#include "module.hpp"
#include "string_utils.hpp"
#include "variant_utils.hpp"

namespace game_logic
{

namespace {
const formula_class& get_class(const std::string& type);

struct property_entry {
	property_entry() {
	}
	property_entry(const std::string& name, variant node) {
		if(node.is_string()) {
			getter = game_logic::formula::create_optional_formula(node);
			return;
		}

		variant variable_setting = node["variable"];
		if(variable_setting.is_bool() && variable_setting.as_bool()) {
			variable = variant(name);
		} else if(variable_setting.is_string()) {
			variable = variable_setting;
		}

		if(node["get"].is_string()) {
			getter = game_logic::formula::create_optional_formula(node["get"]);
		}
		if(node["set"].is_string()) {
			setter = game_logic::formula::create_optional_formula(node["set"]);
		}
	}

	variant variable;
	game_logic::const_formula_ptr getter, setter;
};

typedef std::map<std::string, boost::shared_ptr<formula_class> > classes_map;

}

class formula_class
{
public:
	explicit formula_class(const variant& node);
	void set_name(const std::string& name);
	const std::string& name() const { return name_; }
	const variant& private_data() const { return private_data_; }
	const std::vector<game_logic::const_formula_ptr>& constructor() const { return constructor_; }
	const std::map<std::string, property_entry>& properties() const { return properties_; }
	const classes_map& sub_classes() const { return sub_classes_; }
private:
	std::string name_;
	variant private_data_;
	std::vector<game_logic::const_formula_ptr> constructor_;
	std::map<std::string, property_entry> properties_;
	classes_map sub_classes_;
};

formula_class::formula_class(const variant& node)
{
	std::vector<const formula_class*> bases;
	variant bases_v = node["bases"];
	if(bases_v.is_null() == false) {
		for(int n = 0; n != bases_v.num_elements(); ++n) {
			bases.push_back(&get_class(bases_v[n].as_string()));
		}
	}

	std::map<variant, variant> m;
	private_data_ = variant(&m);

	foreach(const formula_class* base, bases) {
		merge_variant_over(&private_data_, base->private_data_);
	}

	if(node["data"].is_map()) {
		merge_variant_over(&private_data_, node["data"]);
	}

	if(node["constructor"].is_string()) {
		constructor_.push_back(game_logic::formula::create_optional_formula(node["constructor"]));
	}

	foreach(const formula_class* base, bases) {
		for(std::map<std::string, property_entry>::const_iterator i = base->properties_.begin(); i != base->properties_.end(); ++i) {
			properties_[i->first] = i->second;
		}
	}

	const variant properties = node["properties"];
	if(properties.is_map()) {
		foreach(variant key, properties.get_keys().as_list()) {
			const variant prop_node = properties[key];
			property_entry entry(key.as_string(), prop_node);
			properties_[key.as_string()] = entry;
			if(prop_node.has_key("default") && entry.variable.is_string()) {
				private_data_.add_attr(entry.variable, prop_node["default"]);
			}
		}
	}

	const variant classes = node["classes"];
	if(classes.is_map()) {
		foreach(variant key, classes.get_keys().as_list()) {
			const variant class_node = classes[key];
			sub_classes_[key.as_string()].reset(new formula_class(class_node));
		}
	}
}

void formula_class::set_name(const std::string& name)
{
	name_ = name;
	for(classes_map::iterator i = sub_classes_.begin(); i != sub_classes_.end(); ++i) {
		i->second->set_name(name + "." + i->first);
	}
}

namespace
{
struct private_data_scope {
	explicit private_data_scope(int& r, variant* tmp_value=NULL, const variant* value=NULL) : r_(r), tmp_value_(tmp_value) {
		++r_;

		if(tmp_value) {
			*tmp_value = *value;
		}
	}

	~private_data_scope() {
		--r_;

		if(tmp_value_) {
			*tmp_value_ = variant();
		}
	}
private:
	int& r_;
	variant* tmp_value_;
};

classes_map classes_;

const formula_class& get_class(const std::string& type)
{
	if(std::find(type.begin(), type.end(), '.') != type.end()) {
		std::vector<std::string> v = util::split(type, '.');
		const formula_class* c = &get_class(v.front());
		for(int n = 1; n < v.size(); ++n) {
			classes_map::const_iterator itor = c->sub_classes().find(v[n]);
			ASSERT_LOG(itor != c->sub_classes().end(), "COULD NOT FIND FFL CLASS: " << type);
			c = itor->second.get();
		}

		return *c;
	}

	classes_map::const_iterator itor = classes_.find(type);
	if(itor != classes_.end()) {
		return *itor->second;
	}

	const variant v = json::parse_from_file(module::map_file("data/classes/" + type + ".cfg"));
	ASSERT_LOG(v.is_map(), "COULD NOT FIND FFL CLASS: " << type);

	formula_class* const result = new formula_class(v);
	result->set_name(type);
	classes_[type].reset(result);
	return *result;
}

}

formula_object::formula_object(const std::string& type, variant args)
  : class_(&get_class(type))
{
	private_data_ = deep_copy_variant(class_->private_data());
	if(args.is_map()) {
		if(class_->constructor().empty() == false) {
			foreach(const game_logic::const_formula_ptr f, class_->constructor()) {
				private_data_scope scope(expose_private_data_, &tmp_value_, &args);
				execute_command(f->execute(*this));
			}
		} else {
			foreach(const variant& key, args.get_keys().as_list()) {
				set_value(key.as_string(), args[key]);
			}
		}
	}
}

formula_object::formula_object(variant data)
  : class_(&get_class(data["@class"].as_string()))
{
	if(data.is_map()) {
		private_data_ = deep_copy_variant(data);
	} else {
		private_data_ = deep_copy_variant(class_->private_data());
	}

	set_addr(data["_addr"].as_string());
}

formula_object::~formula_object()
{}

variant formula_object::serialize_to_wml() const
{
	variant result = deep_copy_variant(private_data_);
	result.add_attr(variant("@class"), variant(class_->name()));

	char addr_buf[256];
	sprintf(addr_buf, "%p", this);
	result.add_attr(variant("_addr"), variant(addr_buf));
	return result;
}

variant formula_object::get_value(const std::string& key) const
{
	if(expose_private_data_) {
		if(key == "private") {
			return private_data_;
		} else if(key == "value") {
			return tmp_value_;
		}
	}

	std::map<std::string, property_entry>::const_iterator itor = class_->properties().find(key);
	ASSERT_LOG(itor != class_->properties().end(), "UNKNOWN PROPERTY ACCESS " << key << " IN CLASS " << class_->name());

	if(itor->second.getter) {
		private_data_scope scope(expose_private_data_);
		return itor->second.getter->execute(*this);
	} else if(itor->second.variable.is_null() == false) {
		return private_data_[itor->second.variable];
	} else {
		ASSERT_LOG(false, "ILLEGAL READ PROPERTY ACCESS OF NON-READABLE VARIABLE " << key << " IN CLASS " << class_->name());
	}
}

void formula_object::set_value(const std::string& key, const variant& value)
{
	if(expose_private_data_ && key == "private") {
		if(value.is_map() == false) {
			ASSERT_LOG(false, "TRIED TO SET CLASS PRIVATE DATA TO A VALUE WHICH IS NOT A MAP: " << value);
		}
		private_data_ = value;
		return;
	}

	std::map<std::string, property_entry>::const_iterator itor = class_->properties().find(key);
	ASSERT_LOG(itor != class_->properties().end(), "UNKNOWN PROPERTY ACCESS " << key << " IN CLASS " << class_->name());

	if(itor->second.setter) {
		private_data_scope scope(expose_private_data_, &tmp_value_, &value);
		execute_command(itor->second.setter->execute(*this));
	} else if(itor->second.variable.is_null() == false) {
		private_data_.add_attr(itor->second.variable, value);
	} else {
		ASSERT_LOG(false, "ILLEGAL WRITE PROPERTY ACCESS OF NON-WRITABLE VARIABLE " << key << " IN CLASS " << class_->name());
	}
}

void formula_object::get_inputs(std::vector<formula_input>* inputs) const
{
	for(std::map<std::string, property_entry>::const_iterator i = class_->properties().begin(); i != class_->properties().end(); ++i) {
		FORMULA_ACCESS_TYPE type = FORMULA_READ_ONLY;
		if(i->second.getter && i->second.setter || i->second.variable.is_null() == false) {
			type = FORMULA_READ_WRITE;
		} else if(i->second.getter) {
			type = FORMULA_READ_ONLY;
		} else if(i->second.setter) {
			type = FORMULA_WRITE_ONLY;
		} else {
			continue;
		}

		inputs->push_back(formula_input(i->first, type));
	}
}

}