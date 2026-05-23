import { ClassifiedIntent } from './intent-types';

export interface IntentClassifier {
  classify(wavBuffer: Buffer, timezone: string): Promise<ClassifiedIntent>;
}

export const INTENT_CLASSIFIER = Symbol('INTENT_CLASSIFIER');
