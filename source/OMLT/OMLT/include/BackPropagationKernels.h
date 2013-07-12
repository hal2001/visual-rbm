/// random methods

static void NextSeed(const SiCKL::UInt& in_seed, SiCKL::UInt& out_seed)
{
	// calculate next values using Numerical recipes LCRG:  
	// http://en.wikipedia.org/wiki/Linear_congruential_generator  
	const uint32_t A = 1664525;
	const uint32_t C = 1013904223;
	// automatically MOD 2^32
	out_seed = in_seed * A + C;
}

static void NextFloat(const SiCKL::UInt& in_seed, SiCKL::UInt& out_seed, SiCKL::Float& out_float)
{
	NextSeed(in_seed, out_seed);
	/// see http://docs.oracle.com/javase/6/docs/api/java/util/Random.html#nextFloat()
	
	// same as out_seed >> 8 (ie 32 bit to 24 bit int)
	SiCKL::UInt next24 = out_seed / 256u;
	out_float = (Float)next24 / (float)(1 << 24);
}

static void NextGaussian(const SiCKL::UInt& in_seed, SiCKL::UInt& out_seed, SiCKL::Float& out_gaussian)
{
	SiCKL::Float u1 = 0.0f;
	SiCKL::Float u2 = 0.0f;

	// get our random values
	While(u1 == 0.0f)
		NextFloat(in_seed, out_seed, u1);
	EndWhile
	NextFloat(in_seed, out_seed, u2);

	// calculate a normally distributed variable
	const float PI = 3.14159265359f;
	out_gaussian = Sqrt(-2.0f * Log(u1)) * Sin(2.0f * PI * u2);
}

struct SourceCalcEnabledUnits : public SiCKL::Source
{
	float DROPOUT_PROB;

	BEGIN_SOURCE
		BEGIN_CONST_DATA
			CONST_DATA(Buffer2D<UInt>, in_seeds)
		END_CONST_DATA

		BEGIN_OUT_DATA
			OUT_DATA(UInt, out_seed)
			OUT_DATA(Float, out_enabled)
		END_OUT_DATA

		BEGIN_MAIN
			Float prob;
			NextFloat(in_seeds(Index().X, 0), out_seed, prob);

			If(prob > DROPOUT_PROB)
				out_enabled = 1.0f;
			Else
				out_enabled = 0.0f;
			EndIf

		END_MAIN
	END_SOURCE
};

struct SourceFeedForward : public SiCKL::Source
{
	ActivationFunction FUNC;
	float INPUT_DROPOUT_PROB;
	uint32_t INPUT_COUNT;

	BEGIN_SOURCE
		BEGIN_CONST_DATA
			CONST_DATA(Buffer2D<Float>, in_inputs)
			CONST_DATA(Buffer2D<Float>, in_enabled)
			CONST_DATA(Buffer2D<Float>, in_weights);
			CONST_DATA(Buffer2D<UInt>, in_seeds)
		END_CONST_DATA

		BEGIN_OUT_DATA
			OUT_DATA(Float, out_activation)
			OUT_DATA(UInt, out_seed)
		END_OUT_DATA

		BEGIN_MAIN

			// output unit we are calculating
			Int j = Index().X;
			// output vector we're calculating
			Int m = Index().Y;

			// bias
			Float accumulation = 0.0f;
			// calculate dot product between feature and input vector
			ForInRange(i, 0, INPUT_COUNT)
				Float enabled = in_enabled(i, 0);
				Float input = in_inputs(i, m) * enabled;
				// offset i by 1 because of bias column
				Float w_ij = in_weights(i + 1, j);

				accumulation = accumulation + (input * w_ij);
			EndFor
			// take input dropout into account
			accumulation = accumulation * (1.0f / (1.0f - INPUT_DROPOUT_PROB));
			// finally add bias
			accumulation = accumulation + in_weights(0, j);


			// add noise if required
			if(FUNC == NoisySigmoid)
			{
				Float noise;
				NextGaussian(in_seeds(Index().X, Index().Y), out_seed, noise);

				accumulation = accumulation + noise;
			}

			// activation function
			switch(FUNC)
			{
			case Sigmoid:
			case NoisySigmoid:
				out_activation = 1.0f / (1.0f + Exp(-accumulation));
				break;
			case Linear:
				out_activation = accumulation;
			}
		END_MAIN
	END_SOURCE
};

struct SourceCalcTopSensitivities : public SiCKL::Source
{
	ActivationFunction FUNC;
	uint32_t MINIBATCH_SIZE;

	BEGIN_SOURCE
		BEGIN_CONST_DATA
			CONST_DATA(Buffer2D<Float>, in_labels)
			CONST_DATA(Buffer2D<Float>, in_activations)
		END_CONST_DATA

		BEGIN_OUT_DATA
			OUT_DATA(Float, out_sensitivity)
		END_OUT_DATA

		BEGIN_MAIN

			Int j = Index().X;

			out_sensitivity = 0.0f;
			ForInRange(m, 0, MINIBATCH_SIZE)
				Float label = in_labels(j,m);
				Float activation = in_activations(j, m);

				Float d_j = label - activation;
				if(FUNC == NoisySigmoid || FUNC == Sigmoid)
				{
					Float& sigmoid = activation;
					d_j = d_j * ((1.0f - sigmoid) * sigmoid);
				}

				out_sensitivity = out_sensitivity + d_j;
			EndFor
			out_sensitivity = out_sensitivity * (1.0f / (float)MINIBATCH_SIZE);

		END_MAIN
	END_SOURCE
};

struct SourceCalcSensitivities : public SiCKL::Source
{
	ActivationFunction FUNC;
	uint32_t MINIBATCH_SIZE;
	uint32_t NEXT_OUTPUT_COUNT;

	BEGIN_SOURCE
		BEGIN_CONST_DATA
			CONST_DATA(Buffer2D<Float>, in_weights)
			CONST_DATA(Buffer2D<Float>, in_sensitivities)
			CONST_DATA(Buffer2D<Float>, in_activations)
		END_CONST_DATA
			
		BEGIN_OUT_DATA
			OUT_DATA(Float, out_sensitivity)
		END_OUT_DATA

		BEGIN_MAIN
			Int j = Index().X;

			Float dp = 0.0f;
			ForInRange(k, 0, NEXT_OUTPUT_COUNT)
				Float w_jk = in_weights(j + 1, k);
				Float d_k = in_sensitivities(k, 0);

				dp = dp + (w_jk * d_k);
			EndFor

				
			// calculate the average derivative
			if(FUNC == NoisySigmoid || FUNC == Sigmoid)
			{
				Float f_prime = 0.0f;
				ForInRange(m, 0, MINIBATCH_SIZE)
					Float sigmoid = in_activations(j, m);
					f_prime = f_prime + ((1.0f - sigmoid) * sigmoid);
				EndFor

				f_prime = f_prime * (1.0f / MINIBATCH_SIZE);

				out_sensitivity = f_prime * dp;
			}
			else if(FUNC == Linear)
			{
				// average derivative is 1 in this case
				out_sensitivity = dp;
			}

		END_MAIN
	END_SOURCE
};

struct SourceUpdateWeights : public SiCKL::Source
{
	float LEARNING_RATE;
	float MOMENTUM;
	uint32_t MINIBATCH_SIZE;

	BEGIN_SOURCE
		BEGIN_CONST_DATA
			CONST_DATA(Buffer2D<Float>, in_sensitivities)
			CONST_DATA(Buffer2D<Float>, in_inputs)
			CONST_DATA(Buffer2D<Float>, in_enabled_inputs)
			CONST_DATA(Buffer2D<Float>, in_enabled_outputs)
			CONST_DATA(Buffer2D<Float>, in_prev_weights)
			CONST_DATA(Buffer2D<Float>, in_prev_weight_deltas)
		END_CONST_DATA

		BEGIN_OUT_DATA
			OUT_DATA(Float, out_weight)
			OUT_DATA(Float, out_weight_delta)
		END_OUT_DATA

		BEGIN_MAIN
			Int& j = Index().X;
			Int& k = Index().Y;

			Float d_k = in_sensitivities(k, 0);

			// bias update
			If(j == 0 && in_enabled_outputs(k, 0) == 1.0f)
				out_weight_delta = MOMENTUM * in_prev_weight_deltas(j, k) + 
					(1.0f - MOMENTUM) * d_k;
				out_weight = in_prev_weights(j, k) + LEARNING_RATE * out_weight_delta;
			// dropout check
			ElseIf(in_enabled_inputs(j-1, 0) == 1.0f && in_enabled_outputs(k, 0) == 1.0f)
				// get average input 
				Float y_j = 0.0f;
				ForInRange(m, 0, MINIBATCH_SIZE)
					y_j = y_j + in_inputs(j-1, m);
				EndFor
				y_j = y_j * (1.0f / (float)MINIBATCH_SIZE);

				out_weight_delta = MOMENTUM * in_prev_weight_deltas(j, k) + 
					(1.0f - MOMENTUM) * d_k * y_j;

				out_weight = in_prev_weights(j, k) + LEARNING_RATE * out_weight_delta;
			// not bias, not enabled so just copy old values over
			Else
				out_weight_delta = in_prev_weight_deltas(j, k);
				out_weight = in_prev_weights(j, k);
			EndIf

		END_MAIN
	END_SOURCE
};