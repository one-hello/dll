//=======================================================================
// Copyright (c) 2014 Baptiste Wicht
// Distributed under the terms of the MIT License.
// (See accompanying file LICENSE or copy at
//  http://opensource.org/licenses/MIT)
//=======================================================================

#ifndef DLL_DBN_INL
#define DLL_DBN_INL

#include <tuple>

#include "cpp_utils/tuple_utils.hpp"

#include "unit_type.hpp"
#include "dbn_trainer.hpp"
#include "conjugate_gradient.hpp"
#include "dbn_common.hpp"
#include "svm_common.hpp"

namespace dll {

/*!
 * \brief A Deep Belief Network implementation
 */
template<typename Desc>
struct dbn {
    using desc = Desc;
    using this_type = dbn<desc>;

    using tuple_type = typename desc::layers::tuple_type;
    tuple_type tuples;

    static constexpr const std::size_t layers = desc::layers::layers;

    template <std::size_t N>
    using rbm_type = typename std::tuple_element<N, tuple_type>::type;

    //TODO Could be good to ensure that either a) all rbm have the same weight b) use the correct type for each rbm
    using weight = typename rbm_type<0>::weight;

    weight learning_rate = 0.77;

    weight initial_momentum = 0.5;      ///< The initial momentum
    weight final_momentum = 0.9;        ///< The final momentum applied after *final_momentum_epoch* epoch
    weight final_momentum_epoch = 6;    ///< The epoch at which momentum change

    weight weight_cost = 0.0002;        ///< The weight cost for weight decay

    weight momentum = 0;                ///< The current momentum

#ifdef DLL_SVM_SUPPORT
    svm::model svm_model;               ///< The learned model
    svm::problem problem;               ///< libsvm is stupid, therefore, you cannot destroy the problem if you want to use the model...
    bool svm_loaded = false;            ///< Indicates if a SVM model has been loaded (and therefore must be saved)
#endif //DLL_SVM_SUPPORT

    //No arguments by default
    dbn(){};

    //No copying
    dbn(const dbn& dbn) = delete;
    dbn& operator=(const dbn& dbn) = delete;

    //No moving
    dbn(dbn&& dbn) = delete;
    dbn& operator=(dbn&& dbn) = delete;

    void display() const {
        std::size_t parameters = 0;

        std::cout << "DBN with " << layers << " layers" << std::endl;

        cpp::for_each(tuples, [&parameters](auto& rbm){
            typedef typename std::remove_reference<decltype(rbm)>::type rbm_t;
            constexpr const auto num_visible = rbm_t::num_visible;
            constexpr const auto num_hidden = rbm_t::num_hidden;

            parameters += num_visible * num_hidden;

            printf("\tRBM: %lu->%lu : %lu parameters\n", num_visible, num_hidden, num_visible * num_hidden);
        });

        std::cout << "Total parameters: " << parameters << std::endl;
    }

    void store(std::ostream& os) const {
        cpp::for_each(tuples, [&os](auto& rbm){
            rbm.store(os);
        });

#ifdef DLL_SVM_SUPPORT
        svm_store(*this, os);
#endif //DLL_SVM_SUPPORT
    }

    void load(std::istream& is){
        cpp::for_each(tuples, [&is](auto& rbm){
            rbm.load(is);
        });

#ifdef DLL_SVM_SUPPORT
        svm_load(*this, is);
#endif //DLL_SVM_SUPPORT
    }

    template<std::size_t N>
    auto layer() -> typename std::add_lvalue_reference<rbm_type<N>>::type {
        return std::get<N>(tuples);
    }

    template<std::size_t N>
    constexpr auto layer() const -> typename std::add_lvalue_reference<typename std::add_const<rbm_type<N>>::type>::type {
        return std::get<N>(tuples);
    }

    template<std::size_t N>
    static constexpr std::size_t num_visible(){
        return rbm_type<N>::num_visible;
    }

    template<std::size_t N>
    static constexpr std::size_t num_hidden(){
        return rbm_type<N>::num_hidden;
    }

    static constexpr std::size_t input_size(){
        return rbm_type<0>::input_size();
    }

    static constexpr std::size_t output_size(){
        return rbm_type<layers - 1>::output_size();
    }

    static std::size_t full_output_size(){
        std::size_t output = 0;
        for_each_type<tuple_type>([&output](auto* rbm){
            output += std::decay_t<std::remove_pointer_t<decltype(rbm)>>::output_size();
        });
        return output;
    }

    /*{{{ Pretrain */

    /*!
     * \brief Pretrain the network by training all layers in an unsupervised
     * manner.
     */
    template<typename Samples>
    void pretrain(const Samples& training_data, std::size_t max_epochs){
        pretrain(training_data.begin(), training_data.end(), max_epochs);
    }

    /*!
     * \brief Pretrain the network by training all layers in an unsupervised
     * manner.
     */
    template<typename Iterator>
    void pretrain(Iterator first, Iterator last, std::size_t max_epochs){
        using training_t = std::vector<etl::dyn_vector<typename std::iterator_traits<Iterator>::value_type::value_type>>;

        using watcher_t = typename desc::template watcher_t<this_type>;

        watcher_t watcher;

        watcher.pretraining_begin(*this);

        //Convert data to an useful form
        training_t data;
        data.reserve(std::distance(first, last));

        std::for_each(first, last, [&data](auto& sample){
            data.emplace_back(sample);
        });

        training_t next_a;
        training_t next_s;

        auto input = std::ref(data);

        cpp::for_each_i(tuples, [&watcher, this, &input, &next_a, &next_s, max_epochs](std::size_t I, auto& rbm){
            typedef typename std::remove_reference<decltype(rbm)>::type rbm_t;
            constexpr const auto num_hidden = rbm_t::num_hidden;

            auto input_size = static_cast<const training_t&>(input).size();

            //Train each layer but the last one
            if(rbm_t::hidden_unit != unit_type::EXP){
                watcher.template pretrain_layer<rbm_t>(*this, I, input_size);

                rbm.template train<
                        training_t,
                        !watcher_t::ignore_sub,                                 //Enable the RBM Watcher or not
                        typename dbn_detail::rbm_watcher_t<watcher_t>::type>    //Replace the RBM watcher if not void
                    (static_cast<const training_t&>(input), max_epochs);

                //Get the activation probabilities for the next level
                if(I < layers - 1){
                    next_a.clear();
                    next_a.reserve(input_size);
                    next_s.clear();
                    next_s.reserve(input_size);

                    for(std::size_t i = 0; i < input_size; ++i){
                        next_a.emplace_back(num_hidden);
                        next_s.emplace_back(num_hidden);
                    }

                    for(size_t i = 0; i < input_size; ++i){
                        rbm.activate_hidden(next_a[i], next_s[i], static_cast<const training_t&>(input)[i], static_cast<const training_t&>(input)[i]);
                    }

                    input = std::ref(next_a);
                }
            }
        });

        watcher.pretraining_end(*this);
    }

    /*}}}*/

    /*{{{ With labels */

    template<typename Samples, typename Labels>
    void train_with_labels(const Samples& training_data, const Labels& training_labels, std::size_t labels, std::size_t max_epochs){
        cpp_assert(training_data.size() == training_labels.size(), "There must be the same number of values than labels");
        cpp_assert(num_visible<layers - 1>() == num_hidden<layers - 2>() + labels, "There is no room for the labels units");

        train_with_labels(training_data.begin(), training_data.end(), training_labels.begin(), training_labels.end(), labels, max_epochs);
    }

    template<typename Iterator, typename LabelIterator>
    void train_with_labels(Iterator first, Iterator last, LabelIterator lfirst, LabelIterator llast, std::size_t labels, std::size_t max_epochs){
        cpp_assert(std::distance(first, last) == std::distance(lfirst, llast), "There must be the same number of values than labels");
        cpp_assert(num_visible<layers - 1>() == num_hidden<layers - 2>() + labels, "There is no room for the labels units");

        using training_t = std::vector<etl::dyn_vector<typename std::iterator_traits<Iterator>::value_type::value_type>>;

        //Convert data to an useful form
        training_t data;
        data.reserve(std::distance(first, last));

        std::for_each(first, last, [&data](auto& sample){
            data.emplace_back(sample);
        });

        auto input = std::cref(data);

        cpp::for_each_i(tuples, [&input, llast, lfirst, labels, max_epochs](size_t I, auto& rbm){
            typedef typename std::remove_reference<decltype(rbm)>::type rbm_t;
            constexpr const auto num_hidden = rbm_t::num_hidden;

            static training_t next;

            next.reserve(static_cast<const training_t&>(input).size());

            rbm.train(static_cast<const training_t&>(input), max_epochs);

            if(I < layers - 1){
                auto append_labels = (I + 1 == layers - 1);

                for(auto& training_item : static_cast<const training_t&>(input)){
                    etl::dyn_vector<weight> next_item_a(num_hidden + (append_labels ? labels : 0));
                    etl::dyn_vector<weight> next_item_s(num_hidden + (append_labels ? labels : 0));
                    rbm.activate_hidden(next_item_a, next_item_s, training_item, training_item);
                    next.emplace_back(std::move(next_item_a));
                }

                //If the next layers is the last layer
                if(append_labels){
                    auto it = lfirst;
                    auto end = llast;

                    std::size_t i = 0;
                    while(it != end){
                        auto label = *it;

                        for(size_t l = 0; l < labels; ++l){
                            next[i][num_hidden + l] = label == l ? 1.0 : 0.0;
                        }

                        ++i;
                        ++it;
                    }
                }
            }

            input = std::cref(next);
        });
    }

    template<typename TrainingItem>
    size_t predict_labels(const TrainingItem& item_data, std::size_t labels){
        cpp_assert(num_visible<layers - 1>() == num_hidden<layers - 2>() + labels, "There is no room for the labels units");

        etl::dyn_vector<weight> item(item_data);

        etl::dyn_vector<weight> output_a(num_visible<layers - 1>());
        etl::dyn_vector<weight> output_s(num_visible<layers - 1>());

        auto input_ref = std::cref(item);

        cpp::for_each_i(tuples, [labels,&input_ref,&output_a,&output_s](size_t I, auto& rbm){
            typedef typename std::remove_reference<decltype(rbm)>::type rbm_t;
            constexpr const auto num_hidden = rbm_t::num_hidden;

            auto& input = static_cast<const etl::dyn_vector<weight>&>(input_ref);

            if(I == layers - 1){
                static etl::dyn_vector<weight> h1_a(num_hidden);
                static etl::dyn_vector<weight> h1_s(num_hidden);

                rbm.activate_hidden(h1_a, h1_s, input, input);
                rbm.activate_visible(h1_a, h1_s, output_a, output_s);
            } else {
                static etl::dyn_vector<weight> next_a(num_hidden);
                static etl::dyn_vector<weight> next_s(num_hidden);
                static etl::dyn_vector<weight> big_next_a(num_hidden + labels);

                rbm.activate_hidden(next_a, next_s, input, input);

                //If the next layers is the last layer
                if(I + 1 == layers - 1){
                    for(std::size_t i = 0; i < next_a.size(); ++i){
                        big_next_a[i] = next_a[i];
                    }

                    for(size_t l = 0; l < labels; ++l){
                        big_next_a[num_hidden + l] = 0.1;
                    }

                    input_ref = std::cref(big_next_a);
                } else {
                    input_ref = std::cref(next_a);
                }

            }
        });

        size_t label = 0;
        weight max = 0;
        for(size_t l = 0; l < labels; ++l){
            auto value = output_a[num_visible<layers - 1>() - labels + l];

            if(value > max){
                max = value;
                label = l;
            }
        }

        return label;
    }

    /*}}}*/

    /*{{{ Predict */

    template<typename Sample, typename Output>
    void activation_probabilities(const Sample& item_data, Output& result){
        using training_t = etl::dyn_vector<weight>;
        training_t item(item_data);

        auto input = std::cref(item);

        cpp::for_each_i(tuples, [&item, &input, &result](std::size_t I, auto& rbm){
            if(I != layers - 1){
                typedef typename std::remove_reference<decltype(rbm)>::type rbm_t;
                constexpr const auto num_hidden = rbm_t::num_hidden;

                static etl::dyn_vector<weight> next(num_hidden);
                static etl::dyn_vector<weight> next_s(num_hidden);

                rbm.activate_hidden(next, next_s, static_cast<const training_t&>(input), static_cast<const training_t&>(input));

                input = std::cref(next);
            }
        });

        constexpr const auto num_hidden = rbm_type<layers - 1>::num_hidden;

        static etl::dyn_vector<weight> next_s(num_hidden);

        layer<layers - 1>().activate_hidden(result, next_s, static_cast<const training_t&>(input), static_cast<const training_t&>(input));
    }

    template<typename Sample>
    etl::dyn_vector<weight> activation_probabilities(const Sample& item_data){
        etl::dyn_vector<weight> result(output_size());

        activation_probabilities(item_data, result);

        return result;
    }

    template<typename Sample, typename Output>
    void full_activation_probabilities(const Sample& item_data, Output& result){
        using training_t = etl::dyn_vector<weight>;
        training_t item(item_data);

        std::size_t i = 0;

        auto input = std::cref(item);

        cpp::for_each_i(tuples, [&i,&item, &input, &result](std::size_t I, auto& rbm){
            if(I != layers - 1){
                typedef typename std::remove_reference<decltype(rbm)>::type rbm_t;
                constexpr const auto num_hidden = rbm_t::num_hidden;

                static etl::dyn_vector<weight> next_a(num_hidden);
                static etl::dyn_vector<weight> next_s(num_hidden);

                rbm.activate_hidden(next_a, next_s, static_cast<const training_t&>(input), static_cast<const training_t&>(input));

                for(auto& value : next_a){
                    result[i++] = value;
                }

                input = std::cref(next_a);
            }
        });

        constexpr const auto num_hidden = rbm_type<layers - 1>::num_hidden;

        static etl::dyn_vector<weight> next_a(num_hidden);
        static etl::dyn_vector<weight> next_s(num_hidden);

        layer<layers - 1>().activate_hidden(next_a, next_s, static_cast<const training_t&>(input), static_cast<const training_t&>(input));

        for(auto& value : next_a){
            result[i++] = value;
        }
    }

    template<typename Sample>
    etl::dyn_vector<weight> full_activation_probabilities(const Sample& item_data){
        etl::dyn_vector<weight> result(full_output_size());

        full_activation_probabilities(item_data, result);

        return result;
    }

    template<typename Weights>
    size_t predict_label(const Weights& result){
        size_t label = 0;
        weight max = 0;
        for(size_t l = 0; l < result.size(); ++l){
            auto value = result[l];

            if(value > max){
                max = value;
                label = l;
            }
        }

        return label;
    }

    template<typename Sample>
    size_t predict(const Sample& item){
        auto result = activation_probabilities(item);
        return predict_label(result);;
    }

    /*}}}*/

    /*{{{ Fine-tuning */

    template<typename Samples, typename Labels>
    weight fine_tune(const Samples& training_data, Labels& labels, size_t max_epochs, size_t batch_size){
        return fine_tune(training_data.begin(), training_data.end(), labels.begin(), labels.end(), max_epochs, batch_size);
    }

    template<typename Iterator, typename LIterator>
    weight fine_tune(Iterator&& first, Iterator&& last, LIterator&& lfirst, LIterator&& llast, size_t max_epochs, size_t batch_size){
        dll::dbn_trainer<this_type> trainer;
        return trainer.train(*this,
            std::forward<Iterator>(first), std::forward<Iterator>(last),
            std::forward<LIterator>(lfirst), std::forward<LIterator>(llast),
            max_epochs, batch_size);
    }

    /*}}}*/

#ifdef DLL_SVM_SUPPORT

    /*{{{ SVM Training and prediction */

    template<typename Samples, typename Labels>
    bool svm_train(const Samples& training_data, const Labels& labels, const svm_parameter& parameters = default_svm_parameters()){
        return dll::svm_train(*this, training_data, labels, parameters);
    }

    template<typename Iterator, typename LIterator>
    bool svm_train(Iterator&& first, Iterator&& last, LIterator&& lfirst, LIterator&& llast, const svm_parameter& parameters = default_svm_parameters()){
        return dll::svm_train(*this,
            std::forward<Iterator>(first), std::forward<Iterator>(last),
            std::forward<LIterator>(lfirst), std::forward<LIterator>(llast),
            parameters);
    }

    template<typename Samples, typename Labels>
    bool svm_grid_search(const Samples& training_data, const Labels& labels, std::size_t n_fold = 5, const svm::rbf_grid& g = svm::rbf_grid()){
        return dll::svm_grid_search(*this, training_data, labels, n_fold, g);
    }

    template<typename Iterator, typename LIterator>
    bool svm_grid_search(Iterator&& first, Iterator&& last, LIterator&& lfirst, LIterator&& llast, std::size_t n_fold = 5, const svm::rbf_grid& g = svm::rbf_grid()){
        return dll::svm_grid_search(*this,
            std::forward<Iterator>(first), std::forward<Iterator>(last),
            std::forward<LIterator>(lfirst), std::forward<LIterator>(llast),
            n_fold, g);
    }

    template<typename Sample>
    double svm_predict(const Sample& sample){
        return dll::svm_predict(*this, sample);
    }

    /*}}}*/

#endif //DLL_SVM_SUPPORT

};

} //end of namespace dll

#endif
