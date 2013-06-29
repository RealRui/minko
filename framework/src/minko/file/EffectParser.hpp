#pragma once

#include "minko/Common.hpp"
#include "minko/Signal.hpp"

namespace
{
	using namespace minko::render::context;
	using namespace minko::render;
}

namespace minko
{
	namespace file
	{
		class EffectParser :
			public std::enable_shared_from_this<EffectParser>
		{
		public:
			typedef std::shared_ptr<EffectParser>	ptr;

		private:
			typedef std::shared_ptr<Loader> LoaderPtr;

		private:
			std::shared_ptr<Effect>									_effect;
			
			unsigned int											_numDependencies;
			unsigned int											_numLoadedDependencies;

			std::shared_ptr<AbstractContext>						_context;
			std::vector<std::pair<std::string, std::string>>		_programs;
			std::unordered_map<std::string, std::string>			_bindings;
			std::string												_dependenciesCode;

			std::unordered_map<LoaderPtr, Signal<LoaderPtr>::cd>	_loaderCompleteCds;
			std::unordered_map<LoaderPtr, Signal<LoaderPtr>::cd>	_loaderErrorCds;

			std::shared_ptr<Signal<ptr>>							_complete;

		public:
			inline static
			ptr
			create()
			{
				return std::shared_ptr<EffectParser>(new EffectParser());
			}

			inline
			std::shared_ptr<Effect>
			effect()
			{
				return _effect;
			}

			inline
			std::shared_ptr<Signal<ptr>>
			complete()
			{
				return _complete;
			}

			void
			parse(std::shared_ptr<Options>	options,
				  const std::vector<char>&	data);

			void
			dependencyCompleteHandler(std::shared_ptr<Loader> loader);

			void
			dependencyErrorHandler(std::shared_ptr<Loader> loader);

			void
			finalize();

		private:
			EffectParser();
		};
	}
}